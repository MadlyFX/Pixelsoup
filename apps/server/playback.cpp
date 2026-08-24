#include "playback.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/net/frame_commit_sink.hpp>
#include <mediaserver/output/feed_map_model.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/show/demo_edit_scene.hpp>
#include <mediaserver/show/demo_feed_map.hpp>
#include <mediaserver/show/demo_scene.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/test_pattern.hpp>
#include <mediaserver/show/transport_author.hpp>
#include <mediaserver/show/transport_authority.hpp>

#if defined(MEDIASERVER_HAS_GUI)
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/ui/sdl_presenter.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace mediaserver::app {
namespace {

using Clock = show::ShowClock::Clock;

// Shutdown latency ceiling. A boundary further away than this is slept to in
// steps so an interrupt is still observed promptly.
constexpr std::chrono::milliseconds max_sleep{10};

// Bound on the frames one Director iteration will seal after a late wake-up.
constexpr core::u64 max_director_catch_up = 8U;

// Barrier records are diagnostic slots that must be retired once their cohort
// completes; this is the backlog the Director tolerates before refusing to
// seal further.
constexpr std::size_t max_tracked_frames = 64U;

// Schedules the built-in scene as one transaction of node creations applied at
// `apply_at`. Every edit is an ordinary command, so it replicates, journals and
// undoes through the paths everything else does - there is no second mechanism
// for 3D. Returns false if the scheduler refuses any edit, which is a
// programming error here rather than a runtime condition.
[[nodiscard]] bool author_scene_document(
    cmd::DirectorScheduler& scheduler,
    cmd::FrameNumber apply_at) {
    const cmd::UndoContextId undo_context{1U};
    const cmd::TransactionId transaction{1U};
    for (const auto& node : show::demo_scene()) {
        const auto node_id = node.id;
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{node},
            cmd::EraseNode{node_id},
            undo_context,
            transaction,
            apply_at);
        if (!scheduled) {
            return false;
        }
    }
    return true;
}

// The built-in feed map, scheduled the same way as the scene: ordinary
// node-creation commands in one transaction, so it replicates, journals, and
// undoes through the same paths. The ids clear the transport's reserved id and
// the demo scene's range, enforced at compile time in demo_feed_map.hpp.
[[nodiscard]] bool author_feed_map_document(
    cmd::DirectorScheduler& scheduler,
    cmd::FrameNumber apply_at) {
    const cmd::UndoContextId undo_context{1U};
    const cmd::TransactionId transaction{1U};
    for (const auto& node : show::demo_feed_map()) {
        const auto node_id = node.id;
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{node},
            cmd::EraseNode{node_id},
            undo_context,
            transaction,
            apply_at);
        if (!scheduled) {
            return false;
        }
    }
    return true;
}

#if defined(MEDIASERVER_HAS_GUI)
// Writes the demo clip as a short image sequence of TGAs to a temp directory and
// returns a '#'-run template the renderer resolves per frame. A stand-in for
// real media the operator would drop in: enough to have something on the wall to
// drag while the show plays. TGA because it needs no encoder library.
[[nodiscard]] std::string write_demo_clip() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / "mediaserver-editclip";
    std::error_code ec;
    fs::create_directories(directory, ec);

    constexpr core::u32 width = 320U;
    constexpr core::u32 height = 180U;
    std::array<core::u8, 18U> header{};
    header[2U] = 2U; // uncompressed true-colour
    header[12U] = static_cast<core::u8>(width & 0xffU);
    header[13U] = static_cast<core::u8>((width >> 8U) & 0xffU);
    header[14U] = static_cast<core::u8>(height & 0xffU);
    header[15U] = static_cast<core::u8>((height >> 8U) & 0xffU);
    header[16U] = 32U;
    header[17U] = 0x28U; // top-left origin, 8 alpha bits

    std::vector<core::u8> pixels(
        static_cast<std::size_t>(width) * height * 4U, 0U);
    for (core::u32 frame = 0U; frame < show::demo_clip_frames; ++frame) {
        const auto bar = (frame * width) / show::demo_clip_frames;
        for (core::u32 y = 0U; y < height; ++y) {
            for (core::u32 x = 0U; x < width; ++x) {
                const auto index = (static_cast<std::size_t>(y) * width + x) * 4U;
                auto r = static_cast<core::u8>((x * 255U) / (width - 1U));
                auto g = static_cast<core::u8>((y * 255U) / (height - 1U));
                auto b = static_cast<core::u8>(
                    (frame * 255U) / (show::demo_clip_frames - 1U));
                if (x >= bar && x < bar + 14U) {
                    r = 255U;
                    g = 255U;
                    b = 255U;
                }
                pixels[index + 0U] = b; // TGA is BGRA
                pixels[index + 1U] = g;
                pixels[index + 2U] = r;
                pixels[index + 3U] = 255U;
            }
        }
        char name[32]{};
        std::snprintf(name, sizeof(name), "editclip_%04u.tga", frame);
        std::ofstream out{directory / name, std::ios::binary | std::ios::trunc};
        out.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
        out.write(
            reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
    }
    return (directory / "editclip_####.tga").string();
}

// Authors the edit scene the same way the other demos are authored: one
// transaction of node creations, replicating and undoing through the ordinary
// paths.
[[nodiscard]] bool author_edit_document(
    cmd::DirectorScheduler& scheduler,
    cmd::FrameNumber apply_at,
    const std::string& clip_source) {
    for (const auto& node : show::demo_edit_scene(clip_source)) {
        const auto node_id = node.id;
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{node},
            cmd::EraseNode{node_id},
            cmd::UndoContextId{1U},
            cmd::TransactionId{1U},
            apply_at);
        if (!scheduled) {
            return false;
        }
    }
    return true;
}

// A deterministic node id for the channel driving one axis (0=x, 1=y, 2=z) of
// `target`. Deterministic so keying finds-or-creates a channel without scanning
// the document; the high bit keeps it clear of authored node ids.
[[nodiscard]] doc::NodeId channel_node_id(
    doc::NodeId target, core::u64 axis) noexcept {
    constexpr core::u64 channel_id_base = 0x7C00'0000'0000'0000ULL;
    return doc::NodeId{channel_id_base | (target.value() << 4U) | axis};
}

[[nodiscard]] doc::Node make_channel_node(
    doc::NodeId id, doc::NodeId target, std::string_view component) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{scene::schema::channel_type};
    node.properties.insert_or_assign(
        std::string{scene::schema::channel_target},
        doc::PropertyValue{static_cast<std::int64_t>(target.value())});
    node.properties.insert_or_assign(
        std::string{scene::schema::channel_component},
        doc::PropertyValue{std::string{component}});
    return node;
}

// Inserts or updates the key at `frame` in a channel node's indexed key
// properties. Keys are appended contiguously and never individually removed
// (Clear erases the whole channel), so the first missing index is the next free
// slot and a matching frame just has its value replaced.
void upsert_channel_key(doc::Node& node, core::u64 frame, double value) {
    const std::string prefix{scene::schema::channel_key_prefix};
    const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
    const std::string value_suffix{scene::schema::channel_key_value_suffix};
    core::u64 next_index = 0U;
    for (core::u64 i = 0U;; ++i) {
        const auto found = node.properties.find(
            prefix + std::to_string(i) + frame_suffix);
        if (found == node.properties.end()) {
            next_index = i;
            break;
        }
        const auto* const held = std::get_if<std::int64_t>(&found->second);
        if (held != nullptr && *held == static_cast<std::int64_t>(frame)) {
            node.properties.insert_or_assign(
                prefix + std::to_string(i) + value_suffix,
                doc::PropertyValue{value});
            return;
        }
    }
    const auto index = std::to_string(next_index);
    node.properties.insert_or_assign(
        prefix + index + frame_suffix,
        doc::PropertyValue{static_cast<std::int64_t>(frame)});
    node.properties.insert_or_assign(
        prefix + index + value_suffix, doc::PropertyValue{value});
    node.properties.insert_or_assign(
        prefix + index + std::string{scene::schema::channel_key_interp_suffix},
        doc::PropertyValue{std::string{scene::schema::interp_linear}});
}

// Authors a keyframe (or clears one target's position channels) as ordinary
// scene.channel commands - one per axis, all under the one gesture so the whole
// key is a single undo step, on the same scene undo context as moves.
[[nodiscard]] bool schedule_key_edit(
    cmd::DirectorScheduler& scheduler,
    const editor::KeyIntent& key,
    cmd::FrameNumber apply_at) {
    const std::array<std::pair<std::string_view, double>, 3U> axes{{
        {scene::schema::position_x, static_cast<double>(key.position.x)},
        {scene::schema::position_y, static_cast<double>(key.position.y)},
        {scene::schema::position_z, static_cast<double>(key.position.z)},
    }};
    for (core::u64 axis = 0U; axis < axes.size(); ++axis) {
        const auto id = channel_node_id(key.target, axis);
        const auto* const existing = scheduler.projected_document().get(id);
        if (key.clear) {
            if (existing == nullptr) {
                continue;
            }
            auto scheduled = scheduler.schedule_edit(
                cmd::EraseNode{id},
                cmd::PutNode{*existing},
                cmd::UndoContextId{2U},
                cmd::TransactionId{key.gesture},
                apply_at);
            if (!scheduled) {
                return false;
            }
            continue;
        }
        doc::Node node = existing != nullptr
            ? *existing
            : make_channel_node(id, key.target, axes[axis].first);
        upsert_channel_key(node, key.frame, axes[axis].second);
        cmd::Payload inverse = existing != nullptr
            ? cmd::Payload{cmd::PutNode{*existing}}
            : cmd::Payload{cmd::EraseNode{id}};
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{std::move(node)},
            std::move(inverse),
            cmd::UndoContextId{2U},
            cmd::TransactionId{key.gesture},
            apply_at);
        if (!scheduled) {
            return false;
        }
    }
    return true;
}

// Authors one operator move as an ordinary command: read the node's current
// authored state, set its position, and schedule a PutNode whose inverse
// restores the old one. A separate undo context from the transport's, so
// undoing a move never touches playback. The editor produced the position; this
// is the only place a document changes.
[[nodiscard]] bool schedule_scene_edit(
    cmd::DirectorScheduler& scheduler,
    const editor::EditIntent& edit,
    cmd::FrameNumber apply_at) {
    const auto* const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        // The node is gone; there is nothing to move and nothing to fail.
        return true;
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_x},
        doc::PropertyValue{static_cast<double>(edit.position.x)});
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_y},
        doc::PropertyValue{static_cast<double>(edit.position.y)});
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_z},
        doc::PropertyValue{static_cast<double>(edit.position.z)});

    // The transaction is the drag gesture, not the frame: every sealed frame of
    // one continuous drag appends to the same transaction, so the clip moves on
    // the nodes each frame but the whole drag undoes as a single step. A fresh
    // gesture (the next drag) starts a new transaction and becomes the undo top.
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)},
        cmd::PutNode{*existing},
        cmd::UndoContextId{2U},
        cmd::TransactionId{edit.gesture},
        apply_at);
    return static_cast<bool>(scheduled);
}
#endif

[[nodiscard]] bool parse_u64(std::string_view value, core::u64& parsed) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto* const begin = value.data();
    const auto* const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

void sleep_until_boundary(Clock::time_point boundary) noexcept {
    const auto now = Clock::now();
    if (boundary <= now) {
        std::this_thread::yield();
        return;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(boundary - now);
    std::this_thread::sleep_for(std::min(remaining, max_sleep));
}

[[nodiscard]] const char* transport_label(show::TransportState state) noexcept {
    switch (state) {
    case show::TransportState::Playing:
        return "playing";
    case show::TransportState::Paused:
        return "paused";
    case show::TransportState::Stopped:
        break;
    }
    return "stopped";
}

[[nodiscard]] const char* outcome_label(show::FrameCommitOutcome outcome) noexcept {
    switch (outcome) {
    case show::FrameCommitOutcome::Idle:
        return "idle";
    case show::FrameCommitOutcome::Ready:
        return "ready";
    case show::FrameCommitOutcome::Committed:
        return "committed";
    case show::FrameCommitOutcome::NotApproved:
        return "waiting";
    case show::FrameCommitOutcome::CheckpointPending:
        return "checkpoint";
    case show::FrameCommitOutcome::NeedsResync:
        return "resync";
    case show::FrameCommitOutcome::Unavailable:
        break;
    }
    return "disconnected";
}

[[nodiscard]] const char* presenter_error_text(
    show::PresenterError error) noexcept {
    switch (error) {
    case show::PresenterError::InvalidConfiguration:
        return "invalid surface configuration";
    case show::PresenterError::AllocationFailed:
        return "allocation failed";
    case show::PresenterError::DisplayUnavailable:
        break;
    }
    return "the platform refused a window";
}

[[nodiscard]] show::ShowClockConfig clock_config(
    const PlaybackOptions& options) noexcept {
    return show::ShowClockConfig{
        options.rate, options.first_frame, options.last_frame, options.loop};
}

// Per-frame presentation cost, reported only where there is one to report.
// The CPU path has no submission to time and says nothing rather than zero.
[[nodiscard]] std::string gpu_timing_text(
    const show::PresenterTiming& timing) {
    if (!timing.gpu) {
        return {};
    }
    return " present-us=" + std::to_string(timing.last_present_ns / 1'000U)
        + " present-peak-us=" + std::to_string(timing.peak_present_ns / 1'000U)
        + " gpu-observed-us="
        + std::to_string(timing.last_submit_to_observed_ns / 1'000U)
        + " gpu-skipped=" + std::to_string(timing.skipped_presents)
        + " gpu-present=" + (timing.vsync_limited ? "vsync" : "unthrottled");
}

// Control frames the wall clock says are owed but the seal loop has not
// produced. Under an anchored transport a Director that cannot keep up makes
// the show run slow rather than skip positions, so this is the number that
// says so instead of the old skipped-position count.
[[nodiscard]] core::u64 pacer_lag(
    const show::ShowClock& pacer,
    core::u64 sealed) noexcept {
    const auto due = pacer.frame().value();
    return due > sealed ? due - sealed : 0U;
}

[[nodiscard]] core::u64 status_interval(const PlaybackOptions& options) noexcept {
    if (options.status_interval != 0U) {
        return options.status_interval;
    }
    const auto per_second =
        static_cast<core::u64>(show::frames_per_second(options.rate));
    return per_second == 0U ? 1U : per_second;
}

// Builds the output for one role. A windowed request in a build without the
// shell is refused rather than silently downgraded: an operator who asked for a
// window and got a headless run would have no way to tell.
[[nodiscard]] std::unique_ptr<show::Presenter> make_presenter(
    const PlaybackOptions& options,
    const std::string& title,
    bool accepts_commands) {
    if (options.windowed) {
#if defined(MEDIASERVER_HAS_GUI)
        ui::SdlPresenterConfig config{};
        config.title = title;
        config.width = options.surface_width;
        config.height = options.surface_height;
        config.position_x = options.window_x;
        config.position_y = options.window_y;
        config.fullscreen = options.fullscreen;
        config.borderless = options.borderless;
        config.accepts_commands = accepts_commands;
        config.gpu = options.gpu;
        config.scene = options.scene;
        config.output = options.output;

        auto created = ui::SdlPresenter::create(config);
        if (!created) {
            std::cerr << "could not open the output window: "
                      << presenter_error_text(created.error()) << '\n';
            return nullptr;
        }
        return std::move(created).value();
#else
        static_cast<void>(title);
        static_cast<void>(accepts_commands);
        std::cerr << "this build has no window output; configure with "
                     "MEDIASERVER_ENABLE_GUI=ON and the vcpkg gui feature\n";
        return nullptr;
#endif
    }

    auto created = show::HeadlessPresenter::create(
        options.surface_width, options.surface_height);
    if (!created) {
        std::cerr << "could not allocate the presentation surface: "
                  << presenter_error_text(created.error()) << '\n';
        return nullptr;
    }
    return std::move(created).value();
}

// Applies one operator command to the Director transport, re-anchoring it at
// the control frame the result will be sealed into. Every command is exact
// integer state, and the nodes learn about it through the ordinary
// sealed-frame path.
void apply_command(
    show::TransportAuthority& transport,
    show::PresenterCommand command,
    const PlaybackOptions& options,
    cmd::FrameNumber at) noexcept {
    const auto per_second =
        static_cast<core::u64>(show::frames_per_second(options.rate));
    const auto first = options.first_frame.value();
    const auto last = options.last_frame.value();
    const auto current = transport.frame_at(at).value();

    const auto clamped = [first, last](core::u64 target) noexcept {
        return show::ShowFrame{std::min(std::max(target, first), last)};
    };
    const auto back = [current, first](core::u64 delta) noexcept {
        return current - first >= delta ? current - delta : first;
    };
    const auto forward = [current, last](core::u64 delta) noexcept {
        return last - current >= delta ? current + delta : last;
    };

    switch (command) {
    case show::PresenterCommand::TogglePlay:
        if (transport.state() == show::TransportState::Playing) {
            static_cast<void>(transport.pause(at));
        } else {
            static_cast<void>(transport.play(at));
        }
        return;
    case show::PresenterCommand::StepBack:
    case show::PresenterCommand::StepForward:
    case show::PresenterCommand::JumpBack:
    case show::PresenterCommand::JumpForward:
    case show::PresenterCommand::GoToStart:
        break;
    case show::PresenterCommand::None:
    case show::PresenterCommand::Quit:
        return;
    }

    // Scrubbing is a paused operation: stepping while playing would be
    // immediately overwritten by the next boundary.
    if (transport.state() == show::TransportState::Playing) {
        static_cast<void>(transport.pause(at));
    }

    switch (command) {
    case show::PresenterCommand::StepBack:
        static_cast<void>(transport.seek(clamped(back(1U)), at));
        return;
    case show::PresenterCommand::StepForward:
        static_cast<void>(transport.seek(clamped(forward(1U)), at));
        return;
    case show::PresenterCommand::JumpBack:
        static_cast<void>(transport.seek(clamped(back(per_second)), at));
        return;
    case show::PresenterCommand::JumpForward:
        static_cast<void>(transport.seek(clamped(forward(per_second)), at));
        return;
    case show::PresenterCommand::GoToStart:
        static_cast<void>(transport.seek(options.first_frame, at));
        return;
    default:
        return;
    }
}

} // namespace

bool window_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return true;
#else
    return false;
#endif
}

bool gpu_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return ui::gpu_output_available();
#else
    return false;
#endif
}

bool scene_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return ui::scene_output_available();
#else
    return false;
#endif
}

bool output_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return ui::output_output_available();
#else
    return false;
#endif
}

bool editor_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return editor::editor_available();
#else
    return false;
#endif
}

bool parse_frame_rate(std::string_view value, show::FrameRate& rate) noexcept {
    const auto separator = value.find('/');
    core::u64 numerator{};
    core::u64 denominator{1U};
    if (separator == std::string_view::npos) {
        if (!parse_u64(value, numerator)) {
            return false;
        }
    } else if (
        !parse_u64(value.substr(0U, separator), numerator)
        || !parse_u64(value.substr(separator + 1U), denominator)) {
        return false;
    }

    if (numerator == 0U || numerator > show::max_rate_numerator
        || denominator == 0U || denominator > show::max_rate_denominator) {
        return false;
    }
    rate = show::FrameRate{
        static_cast<core::u32>(numerator), static_cast<core::u32>(denominator)};
    return true;
}

bool parse_show_range(
    std::string_view value,
    show::ShowFrame& first,
    show::ShowFrame& last) noexcept {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos) {
        return false;
    }
    core::u64 begin{};
    core::u64 end{};
    if (!parse_u64(value.substr(0U, separator), begin)
        || !parse_u64(value.substr(separator + 1U), end) || end < begin
        || end - begin == std::numeric_limits<core::u64>::max()) {
        return false;
    }
    first = show::ShowFrame{begin};
    last = show::ShowFrame{end};
    return true;
}

bool valid_playback_options(const PlaybackOptions& options) noexcept {
    return show::ShowClock::valid_configuration(clock_config(options))
        && options.surface_width != 0U && options.surface_height != 0U
        && options.surface_width <= 7'680U && options.surface_height <= 4'320U;
}

int run_node_playback(
    net::NodeSessionService& service,
    cmd::NodeCommandBus& node,
    const PlaybackOptions& options,
    const volatile std::sig_atomic_t& stop_requested) {
    // A node output window is input-inert on purpose: what it shows is
    // replicated state, so keyboard transport there would be a lie.
    auto presenter = make_presenter(
        options,
        "media-server node " + std::to_string(options.node_index),
        false);
    if (presenter == nullptr) {
        return 1;
    }

    net::NodeServiceFrameCommitSink sink{service, node};
    show::FrameBoundaryDriverConfig config{};
    config.clock = clock_config(options);
    config.mailbox_capacity = 16U;
    config.max_commits_per_tick = 8U;

    auto created =
        show::FrameBoundaryDriver::create(config, sink, Clock::now());
    if (!created) {
        std::cerr << "invalid node frame-boundary driver configuration\n";
        return 1;
    }
    auto driver = std::move(created).value();

    auto retirement = show::RootRetirementRing::create(32U);
    if (!retirement) {
        std::cerr << "could not allocate the node root retirement ring\n";
        return 1;
    }
    auto retired = std::move(retirement).value();

    // The node clock is a pacer, not the transport: it decides when a boundary
    // occurs, while what is presented comes from the committed document.
    if (!driver.play(Clock::now())) {
        std::cerr << "could not start the node frame pacer\n";
        return 1;
    }

    const auto interval = status_interval(options);
    show::PublishedFrame presented{};
    core::u64 boundaries = 0U;
    core::u64 presented_frames = 0U;
    core::u64 since_status = 0U;
    core::u64 present_failures = 0U;
    auto last_outcome = show::FrameCommitOutcome::Idle;
    auto closed = false;

    // 3D scene state, used only when the presenter renders scenes. The model is
    // reset when the committed document changes and evaluated every frame, so a
    // playing show re-parses nothing and two nodes evaluate byte-identical
    // scenes from the same show frame.
    scene::SceneModel scene_model;
    core::u64 scene_version = 0U;
    bool scene_loaded = false;

    // Feed-map state, used only on the output path. Reset on a document change
    // like the scene, and evaluated the same way, so two nodes derive the same
    // map from the same committed revision.
    output::FeedMapModel feed_model;
    core::u64 feed_version = 0U;
    bool feed_loaded = false;

    while (stop_requested == 0 && !closed) {
        const auto now = Clock::now();
        auto ticked = driver.tick(now);
        if (!ticked) {
            // A stall long enough to invalidate the pacer is a local timing
            // fault, never a reason to present unapproved state. Re-anchor and
            // keep holding whatever the Director last approved.
            if (ticked.error() == show::ShowClockError::TimeOverflow) {
                static_cast<void>(driver.seek(driver.clock().frame(), now));
                continue;
            }
            std::cerr << "node frame pacer refused a time observation (code "
                      << static_cast<unsigned>(ticked.error()) << ")\n";
            return 1;
        }

        const auto report = std::move(ticked).value();
        boundaries += report.boundaries;
        last_outcome = report.last_outcome;

        std::size_t skipped = 0U;
        show::PublishedFrame incoming{};
        if (driver.mailbox().consume_latest(incoming, skipped, &retired)) {
            // The outgoing root is parked rather than released here; the ring
            // is drained below, away from the boundary.
            static_cast<void>(retired.retire(std::move(presented.root)));
            presented = std::move(incoming);
            ++presented_frames;
        }

        const auto transport = presented.transport;
        show::TestPatternInput input{};
        // The position was derived from the anchor at the exact control frame
        // that carried it, so the two can never be a mismatched pair.
        input.show_frame = transport ? presented.show_frame : options.first_frame;
        input.control_frame = presented.control_frame;
        input.rate = transport ? transport->rate : options.rate;
        input.state =
            transport ? transport->state : show::TransportState::Stopped;
        input.node_index = options.node_index;
        input.held = report.held;
        input.needs_resync =
            report.last_outcome == show::FrameCommitOutcome::NeedsResync;

        // A node draws its slice of the replicated scene through its feed map,
        // or draws the raw slice, or shows the test pattern - decided once at
        // start-up by which renderers the presenter was built with. A node that
        // could not build the path it was asked for never reaches here:
        // make_presenter refuses it. The output path implies the scene path,
        // which is the source it transforms.
        const bool render_output = presenter->renders_output();
        const bool render_scene = presenter->renders_scenes();
        scene::Scene scene_frame;
        output::FeedMap feed_map;
        show::PresenterStatus status{show::PresenterStatus::Skipped};
        if (render_scene) {
            if (!scene_loaded || presented.version.value() != scene_version) {
                scene_model.reset(presented.root);
                scene_version = presented.version.value();
                scene_loaded = true;
            }
            const scene::SceneTime time{
                input.show_frame.value(),
                input.rate.numerator,
                input.rate.denominator};
            scene_frame = scene_model.evaluate(time);
        }
        if (render_output) {
            if (!feed_loaded || presented.version.value() != feed_version) {
                feed_model.reset(presented.root);
                feed_version = presented.version.value();
                feed_loaded = true;
            }
            feed_map = feed_model.evaluate();
            // This node's own binding, from the document. A node the feed map
            // does not bind outputs nothing rather than guessing, which reads as
            // obviously unmapped instead of subtly wrong.
            const auto* const map = feed_map.map_for(options.node_index);
            status = map != nullptr
                ? presenter->present(scene_frame, *map)
                : show::PresenterStatus::Skipped;
        } else if (render_scene) {
            // The node's own rectangle of the canvas, from the document. A node
            // the feed map does not bind draws the whole canvas rather than
            // guessing.
            const auto* const bound = scene_frame.slice_for(options.node_index);
            const auto slice =
                bound != nullptr ? *bound : scene::whole(scene_frame.canvas);
            status = presenter->present(scene_frame, slice);
        } else {
            status = presenter->present(input);
        }

        switch (status) {
        case show::PresenterStatus::Closed:
            closed = true;
            break;
        case show::PresenterStatus::Failed:
            ++present_failures;
            break;
        case show::PresenterStatus::Presented:
        case show::PresenterStatus::Skipped:
            break;
        }

        since_status += report.boundaries;
        if (since_status >= interval) {
            since_status = 0U;
            std::cout << "node " << options.node_index
                      << " show-frame=" << input.show_frame.value()
                      << " control-frame=" << presented.control_frame.value()
                      << " version=" << presented.version.value()
                      << " transport=" << transport_label(input.state)
                      << " state=" << outcome_label(report.last_outcome)
                      << " boundaries=" << boundaries
                      << " presented=" << presented_frames
                      << " held=" << driver.counters().held_boundaries
                      << " skipped=" << skipped
                      << " overflows=" << driver.counters().mailbox_overflows
                      // The scene digest compares the replicated state itself,
                      // not a rendering of it, so it agrees across nodes drawing
                      // different slices - which the frame-buffer digest cannot.
                      << " digest="
                      << (render_scene
                              ? scene::digest(scene_frame)
                              : show::frame_buffer_digest(presenter->surface()));
            // The feed-map digest agrees across nodes for the same reason the
            // scene one does: it is the replicated parameters, not a rendering.
            if (render_output) {
                std::cout << " feed=" << output::digest(feed_map);
            }
            std::cout << gpu_timing_text(presenter->timing()) << '\n'
                      << std::flush;
        }

        // Off-boundary housekeeping: this is the only place a superseded trie
        // is actually released.
        static_cast<void>(retired.drain());

        if (options.run_frames != 0U && boundaries >= options.run_frames) {
            break;
        }
        sleep_until_boundary(driver.clock().next_boundary_at());
    }

    std::cout << "node " << options.node_index << " stopping"
              << " show-frame="
              << (presented.transport ? presented.show_frame.value()
                                      : options.first_frame.value())
              << " control-frame=" << presented.control_frame.value()
              << " version=" << presented.version.value()
              << " boundaries=" << boundaries
              << " presented=" << presented_frames
              << " committed=" << driver.counters().committed_frames
              << " held=" << driver.counters().held_boundaries
              << " checkpoints=" << driver.counters().checkpoint_activations
              << " present-failures=" << present_failures
              << " window=" << (presenter->windowed() ? "closed" : "headless")
              << gpu_timing_text(presenter->timing())
              << " last=" << outcome_label(last_outcome) << '\n'
              << std::flush;
    return 0;
}

int run_director_show(
    cmd::DirectorScheduler& scheduler,
    net::DirectorSessionHub& hub,
    net::DirectorFrameBarrier& barrier,
    const PlaybackOptions& options,
    const volatile std::sig_atomic_t& stop_requested) {
    // The editor replaces the preview presenter when asked for: it is the
    // operator's surface, not an output window. Everything else - the seal loop,
    // the transport, replication - is identical either way.
    std::unique_ptr<show::Presenter> presenter;
    std::string clip_source;
#if defined(MEDIASERVER_HAS_GUI)
    std::unique_ptr<editor::EditorShell> editor;
    if (options.edit) {
        editor::EditorConfig editor_config{};
        editor_config.title = "media-server editor";
        // Honour an explicit --surface, but keep the editor usably large when
        // the caller left the small test-pattern default in place.
        editor_config.width =
            options.surface_width >= 1024U ? options.surface_width : 1400U;
        editor_config.height =
            options.surface_height >= 640U ? options.surface_height : 900U;
        editor_config.position_x = options.window_x;
        editor_config.position_y = options.window_y;
        auto created_editor = editor::EditorShell::create(editor_config);
        if (!created_editor) {
            std::cerr << "could not open the operator editor window\n";
            return 1;
        }
        editor = std::move(created_editor).value();
        clip_source = write_demo_clip();
    }
#endif
    if (!options.edit) {
        presenter = make_presenter(options, "media-server director", true);
        if (presenter == nullptr) {
            return 1;
        }
    }

    const auto start = Clock::now();

    // One clock, and it is a pacer. It free-runs regardless of transport state
    // and decides only when the next control frame is due, so pausing never
    // stops the frame stream that carries transport changes and keeps the
    // cohort acknowledging.
    //
    // Where the show is, is not a clock reading at all. It is the replicated
    // anchor evaluated at the control-frame ordinal, which is what makes the
    // Director and every node compute the same position from the same state.
    auto paced = show::ShowClock::create(
        show::ShowClockConfig{
            options.rate,
            show::ShowFrame{0U},
            show::ShowFrame{std::numeric_limits<core::u64>::max() - 1U},
            false},
        start);
    auto created = show::TransportAuthority::create(
        clock_config(options), scheduler.sealed_through());
    if (!paced || !created) {
        std::cerr << "invalid Director show clock configuration\n";
        return 1;
    }
    auto pacer = std::move(paced).value();
    auto transport = std::move(created).value();

    if (!pacer.play(start)) {
        std::cerr << "could not start the Director frame pacer\n";
        return 1;
    }

    // The built-in scene and feed map are authored once, as ordinary
    // node-creation commands on the first frame this Director will seal. From
    // there they are document state like any other: they replicate to the
    // cohort, and a playing show adds no further edits because every motion is a
    // rate the nodes integrate against the show frame themselves, and the feed
    // map does not move at all.
    if (options.scene || options.output || options.edit) {
        const cmd::FrameNumber first{scheduler.sealed_through().value() + 1U};
        if (options.scene && !author_scene_document(scheduler, first)) {
            std::cerr << "could not author the built-in scene document\n";
            return 1;
        }
        if (options.output && !author_feed_map_document(scheduler, first)) {
            std::cerr << "could not author the built-in feed map document\n";
            return 1;
        }
#if defined(MEDIASERVER_HAS_GUI)
        if (options.edit && !author_edit_document(scheduler, first, clip_source)) {
            std::cerr << "could not author the edit scene document\n";
            return 1;
        }
#endif
    }

    std::deque<net::DirectorFrameBarrierId> tracked;
    std::vector<show::PresenterCommand> operator_commands;
#if defined(MEDIASERVER_HAS_GUI)
    // Operator moves from the editor, coalesced to the latest per node, authored
    // at the next frame boundary like the transport commands.
    std::vector<editor::EditIntent> pending_edits;
    // A pending scene-edit undo (its transaction id, or 0 for none), authored at
    // the boundary after the edits so it reverses whatever the edits just left on
    // the stack.
    core::u64 pending_undo{};
    // A pending timeline scrub: the show frame the operator dragged the playhead
    // to, applied to the transport (paused) at the boundary.
    std::optional<core::u64> pending_seek;
    // Pending keyframe requests, authored as scene.channel edits at the boundary.
    std::vector<editor::KeyIntent> pending_keys;
#endif
    std::optional<show::TransportAnchor> authored;
    const auto interval = status_interval(options);
    core::u64 sealed = 0U;
    core::u64 since_status = 0U;
    core::u64 anchors = 0U;
    core::u64 present_failures = 0U;
    auto autoplay_pending = options.autoplay;
    auto closed = false;

    // The Director previews the whole canvas of the same replicated scene. It
    // evaluates from its own committed document, so it needs no mailbox; the
    // model is reset when that document's version changes.
    scene::SceneModel scene_model;
    core::u64 scene_version = 0U;
    bool scene_loaded = false;

    // The feed map, evaluated only to report its digest: the Director previews
    // the whole canvas, not a node's slice, so it does not transform. Reported
    // so an operator can confirm the Director and every node agree on the map.
    output::FeedMapModel feed_model;
    core::u64 feed_version = 0U;
    bool feed_loaded = false;

    while (stop_requested == 0 && !closed) {
        const auto now = Clock::now();

        auto paced_tick = pacer.advance_to(now);
        if (!paced_tick) {
            if (paced_tick.error() == show::ShowClockError::TimeOverflow) {
                static_cast<void>(pacer.seek(pacer.frame(), now));
                continue;
            }
            std::cerr << "Director frame pacer refused a time observation\n";
            return 1;
        }

        const auto due = std::min(
            std::move(paced_tick).value().advanced, max_director_catch_up);
        for (core::u64 index = 0U; index < due; ++index) {
            if (tracked.size() >= max_tracked_frames) {
                std::cerr << "Director cohort backlog reached " << tracked.size()
                          << " unretired frames; refusing to seal further\n";
                return 1;
            }

            const auto through = scheduler.sealed_through().value();
            if (through == std::numeric_limits<core::u64>::max()) {
                std::cerr << "Director control frame numbering is exhausted\n";
                return 1;
            }
            const cmd::FrameNumber frame{through + 1U};

            // Operator input is applied to the first frame of this iteration,
            // because an anchor has to name the exact control frame it takes
            // effect on.
            if (index == 0U) {
                if (autoplay_pending) {
                    autoplay_pending = false;
                    if (!transport.play(frame)) {
                        std::cerr << "could not start the Director transport\n";
                        return 1;
                    }
                }
                for (const auto command : operator_commands) {
                    apply_command(transport, command, options, frame);
                }
                operator_commands.clear();
#if defined(MEDIASERVER_HAS_GUI)
                for (const auto& edit : pending_edits) {
                    if (!schedule_scene_edit(scheduler, edit, frame)) {
                        std::cerr << "could not author a scene edit for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_edits.clear();
                for (const auto& key : pending_keys) {
                    if (!schedule_key_edit(scheduler, key, frame)) {
                        std::cerr << "could not author a keyframe for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_keys.clear();
                if (pending_seek) {
                    // Scrubbing is a paused operation, like the keyboard step and
                    // jump commands: pause, then seek to the dragged frame,
                    // clamped to the show range. The new anchor replicates like
                    // any other transport change.
                    if (transport.state() == show::TransportState::Playing) {
                        static_cast<void>(transport.pause(frame));
                    }
                    const auto target = std::min(
                        std::max(*pending_seek, options.first_frame.value()),
                        options.last_frame.value());
                    static_cast<void>(
                        transport.seek(show::ShowFrame{target}, frame));
                    pending_seek.reset();
                }
                if (pending_undo != 0U) {
                    // Undo the top of the scene-edit context - a whole drag's
                    // worth of commands, reversed as one transaction. NoHistory
                    // just means there was nothing to undo, which is not an error.
                    auto undone = scheduler.schedule_undo(
                        cmd::UndoContextId{2U},
                        cmd::TransactionId{pending_undo},
                        frame);
                    if (!undone
                        && undone.error()
                            != cmd::DirectorUndoError::NoHistory) {
                        std::cerr << "could not author a scene-edit undo for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                    pending_undo = 0U;
                }
#endif
            }
            static_cast<void>(transport.settle(frame));

            // The anchor travels as an ordinary command inside a sealed frame,
            // so every node derives the position from the same revision. An
            // unchanged anchor authors nothing, so a playing show costs the
            // document no edits at all -- which is what removes the per-frame
            // durable write behind it.
            if (!authored || *authored != transport.anchor()) {
                const auto command = show::schedule_transport_anchor(
                    scheduler,
                    transport.anchor(),
                    frame,
                    cmd::UndoContextId{1U},
                    cmd::TransactionId{frame.value()});
                if (!command) {
                    std::cerr
                        << "could not author the transport anchor for frame "
                        << frame.value() << " (code "
                        << static_cast<unsigned>(command.error()) << ")\n";
                    return 1;
                }
                authored = transport.anchor();
                ++anchors;
            }

            auto sealed_frame = barrier.seal_and_track(hub, frame);
            if (!sealed_frame) {
                std::cerr << "Director could not seal frame " << frame.value()
                          << " (code "
                          << static_cast<unsigned>(sealed_frame.error().code)
                          << ")\n";
                return 1;
            }
            tracked.push_back(sealed_frame.value().barrier);
            ++sealed;
        }

        // Retire in order. A frame whose cohort has not finished stays, and so
        // does everything behind it, which is what bounds the backlog above.
        while (!tracked.empty() && barrier.retire(tracked.front())) {
            tracked.pop_front();
        }

        const auto control_frame = scheduler.sealed_through();
        show::TestPatternInput input{};
        input.show_frame = transport.frame_at(control_frame);
        input.control_frame = control_frame;
        input.rate = options.rate;
        input.state = transport.state();
        input.node_index = options.node_index;
        input.held = due == 0U;

        // The Director previews the whole canvas of the replicated scene, so it
        // shows what the wall shows between its nodes. It evaluates from its own
        // committed document rather than a mailbox.
        const bool render_scene =
            presenter != nullptr && presenter->renders_scenes();
        // The scene is evaluated for the preview or the editor: both work on the
        // same committed scene at the show frame.
        const bool needs_scene = render_scene || options.edit;
        scene::Scene scene_frame;
        output::FeedMap feed_map;
        show::PresenterStatus status{show::PresenterStatus::Skipped};
        if (needs_scene) {
            const auto& document = scheduler.sealed_document();
            if (!scene_loaded || document.version().value() != scene_version) {
                scene_model.reset(document);
                scene_version = document.version().value();
                scene_loaded = true;
            }
            const scene::SceneTime time{
                input.show_frame.value(),
                input.rate.numerator,
                input.rate.denominator};
            scene_frame = scene_model.evaluate(time);
        }

        bool handled = false;
#if defined(MEDIASERVER_HAS_GUI)
        if (options.edit && editor) {
            handled = true;
            const editor::EditorTimeline timeline{
                input.show_frame.value(),
                options.first_frame.value(),
                options.last_frame.value(),
                transport.state() == show::TransportState::Playing};
            const auto keyframes = scene_model.channel_keyframes();
            auto editor_frame = editor->frame(scene_frame, timeline, keyframes);
            if (editor_frame.status == editor::EditorStatus::Closed) {
                closed = true;
            }
            // Coalesce to the latest edit per node before the next boundary
            // authors them, so a drag is one command per frame, not per event.
            for (const auto& edit : editor_frame.edits) {
                bool merged = false;
                for (auto& queued : pending_edits) {
                    if (queued.node == edit.node) {
                        queued = edit;
                        merged = true;
                        break;
                    }
                }
                if (!merged) {
                    try {
                        pending_edits.push_back(edit);
                    } catch (const std::bad_alloc&) {
                        std::cerr << "could not queue a scene edit\n";
                        return 1;
                    }
                }
            }
            if (editor_frame.undo_transaction != 0U) {
                pending_undo = editor_frame.undo_transaction;
            }
            if (editor_frame.seek_to_frame) {
                pending_seek = editor_frame.seek_to_frame;
            }
            for (const auto& key : editor_frame.keys) {
                try {
                    pending_keys.push_back(key);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue a keyframe\n";
                    return 1;
                }
            }
        }
#endif
        if (!handled) {
            status = render_scene
                ? presenter->present(scene_frame, scene::whole(scene_frame.canvas))
                : presenter->present(input);
        }

        // Evaluated for its digest only, so the Director's status line can show
        // it agrees with the nodes' feed maps.
        if (options.output) {
            const auto& document = scheduler.sealed_document();
            if (!feed_loaded || document.version().value() != feed_version) {
                feed_model.reset(document);
                feed_version = document.version().value();
                feed_loaded = true;
            }
            feed_map = feed_model.evaluate();
        }

        switch (status) {
        case show::PresenterStatus::Closed:
            closed = true;
            break;
        case show::PresenterStatus::Failed:
            ++present_failures;
            break;
        case show::PresenterStatus::Presented:
        case show::PresenterStatus::Skipped:
            break;
        }

        if (presenter != nullptr) {
            for (auto command = presenter->take_command();
                 command != show::PresenterCommand::None;
                 command = presenter->take_command()) {
                try {
                    operator_commands.push_back(command);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue an operator transport command\n";
                    return 1;
                }
            }
        }

        since_status += due;
        if (due != 0U && since_status >= interval) {
            since_status = 0U;
            std::cout << "director show-frame=" << input.show_frame.value()
                      << " control-frame=" << control_frame.value()
                      << " transport=" << transport_label(transport.state())
                      << " sealed=" << sealed << " anchors=" << anchors
                      << " pacer-lag=" << pacer_lag(pacer, sealed)
                      << " tracked=" << tracked.size()
                      << " sessions=" << hub.active_subscriptions()
                      << " released-through="
                      << barrier.released_through().through_frame.value();
            // The same scene digest the nodes report. Printed here so an
            // operator can confirm the Director and every node agree on the
            // replicated scene at the same show frame.
            if (needs_scene) {
                std::cout << " digest=" << scene::digest(scene_frame);
            }
            // And the same feed-map digest, so the operator can confirm the map
            // itself is replicated, not just the scene.
            if (options.output) {
                std::cout << " feed=" << output::digest(feed_map);
            }
            if (presenter != nullptr) {
                std::cout << gpu_timing_text(presenter->timing());
            }
            std::cout << '\n' << std::flush;
        }

        if (options.run_frames != 0U && sealed >= options.run_frames) {
            break;
        }
        sleep_until_boundary(pacer.next_boundary_at());
    }

    std::cout << "director stopping"
              << " show-frame="
              << transport.frame_at(scheduler.sealed_through()).value()
              << " control-frame=" << scheduler.sealed_through().value()
              << " transport=" << transport_label(transport.state())
              << " sealed=" << sealed << " anchors=" << anchors
              << " pacer-lag=" << pacer_lag(pacer, sealed)
              << " tracked=" << tracked.size()
              << " present-failures=" << present_failures
              << " surface="
              << (options.edit ? "editor"
                               : (presenter && presenter->windowed() ? "window"
                                                                     : "headless"))
              << " released-through="
              << barrier.released_through().through_frame.value() << '\n'
              << std::flush;
    return 0;
}

} // namespace mediaserver::app
