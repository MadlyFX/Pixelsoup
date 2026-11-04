#include "studio_app.hpp"

#include "bake.hpp"
#include "compile.hpp"
#include "compile_worker.hpp"
#include "document_io.hpp"
#include "package_builder.hpp"
#include "page_revision.hpp"
#include "performance.hpp"
#include "preview.hpp"
#include "scene.hpp"
#include "imgui_layer.hpp"
#include "scene_view.hpp"
#include "stage.hpp"
#include "stage_gizmo.hpp"
#include "stage_panel.hpp"
#include "session.hpp"
#include "stage_camera.hpp"
#include "stage_overlay.hpp"
#include "scene_stats.hpp"
#include "timeline.hpp"
#include "stage_pick.hpp"
#include "widgets.hpp"

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/shapes.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/gui/window.hpp>
#include <mediaserver/render/particle_pass.hpp>

#include <tinyfiledialogs.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // windows.h, through d3d12.h, must not define min and max
#endif
#include <d3d12.h>
#include <process.h>
#include <wrl/client.h>
#endif

namespace ajfx::studio {
namespace {

namespace editor = mediaserver::editor;
namespace rhi = mediaserver::rhi;

constexpr float top_bar_height = 52.0F;
constexpr float transport_height = 54.0F;
// The timeline under the transport: a row a track, up to this many before it scrolls.
constexpr float track_height = 24.0F;
constexpr std::size_t tracks_shown = 6U;
constexpr std::size_t history_limit = 300U;
// While a value is dragged, a new package at most this often, and only once
// the preview has taken in the last: each package restarts the effect and
// replays it to the playhead, and handing the renderer packages faster than
// it adopts them piles up instances and hitches the preview.
constexpr double submit_interval = 0.12;
// Frames the preview draws a package undisturbed before a dragged value
// sends the next, and the longest a drag waits for that.
constexpr std::uint32_t settle_frames = 3U;
constexpr double settle_timeout = 0.75;
// A scene bakes its solid world by itself once nothing has changed for this
// long and the preview is up to date.
constexpr double auto_bake_delay = 0.8;
// An act is stamped as the renderer expects one it need not replay for.
static_assert(act_tick(true, 10U, preview_acting_lead) ==
              mediaserver::render::acting_tick(10U, preview_acting_lead));

gui::FontAtlas load_font(float scale) {
    constexpr std::array<const char*, 3> candidates{"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                                                    "C:/Windows/Fonts/tahoma.ttf"};
    constexpr std::array<float, 3> sizes{0.86F, 1.2F, 1.75F};
    for (const char* path : candidates) {
        if (auto atlas = gui::FontAtlas::create(path, 14U, scale, sizes)) {
            return std::move(*atlas);
        }
    }
    return gui::FontAtlas::empty();
}

std::filesystem::path preview_package_path() {
    std::error_code error;
    std::filesystem::path directory = std::filesystem::temp_directory_path(error) / "ajfx-studio";
    std::filesystem::create_directories(directory, error);
    // A Studio that did not close cleanly leaves its preview behind; any a
    // day old is nobody's.
    const auto stale = std::filesystem::file_time_type::clock::now() - std::chrono::hours{24};
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        std::error_code ignored;
        if (entry.path().extension() == ".ajfx" && entry.last_write_time(ignored) < stale) {
            std::filesystem::remove(entry.path(), ignored);
        }
    }
#if defined(_WIN32)
    const int process = _getpid();
#else
    const int process = 0;
#endif
    return directory / ("preview-" + std::to_string(process) + ".ajfx");
}

struct Toast final {
    std::string text;
    bool ok{true};
    double until{0.0};
};

class Studio final {
public:
    Studio(gui::UiWindow& window, gui::FontAtlas& font, gui::InputState& input, gui::UiDrawList& list,
           gui::UiContext& ctx)
        : window_{window}, font_{font}, input_{input}, list_{list}, ctx_{ctx},
          builder_{preview_package_path(), preview_instance_seed()} {}

    ~Studio() {
        std::error_code error;
        std::filesystem::remove(builder_.preview_path(), error);
    }

    bool start(const StudioOptions& options, SessionState session) {
        std::string error;
        preview_ = Preview::create(window_.device(), error);
        if (!preview_) {
            preview_error_ = error;
        }
        session_ = std::move(session);
        // The window's panels and overlays as they were left.
        show_guides_ = session_.guides;
        show_handles_ = session_.handles;
        show_paths_ = session_.paths;
        figure_ = session_.figure;
        night_ = session_.night;
        looping_ = session_.looping;

        const bool resumed = options.open.empty() && options.restore_page && resume_page();
        if (!resumed && (options.open.empty() || !open_path(options.open))) {
            replace_document(new_scene(), {});
        }
        looping_ = looping_ && !options.once;
        show_guides_ = options.guides.value_or(show_guides_);
        show_handles_ = options.guides.value_or(show_handles_);
        show_paths_ = options.paths.value_or(show_paths_);
        if (options.lose_device >= 0.0F) {
            lose_device_at_ = window_.time_seconds() + static_cast<double>(options.lose_device);
        }
        if (options.at >= 0.0F) {
            frame_ = static_cast<double>(options.at) * 60.0;
            playing_ = false;
        }
        scene_tree_split_ = session_.scene_tree_split;
        scene_inspector_split_ = session_.scene_inspector_split;
        scene_view_.restore(session_.scene_selection, session_.scene_folded);
        std::string ini;
        if (const std::filesystem::path file = session_path(); !file.empty()) {
            ini = (file.parent_path() / "windows.ini").string();
        }
        imgui_ = std::make_unique<ImguiLayer>(window_, std::move(ini));
        return true;
    }

    // One frame. Returns false once the window should close.
    bool frame() {
        if (!window_.pump(input_)) {
            const bool changed = unsaved();
            if (confirm_discard("close ajfx Studio")) {
                keep_session(changed);
                return false;
            }
            window_.cancel_close_request();
        }
        if (const float scale = window_.ui_scale(); std::fabs(scale - font_.scale()) > 0.001F) {
            font_ = load_font(scale);
            list_.set_white_texel(font_.white_u(), font_.white_v());
            static_cast<void>(window_.set_atlas(font_));
        }
        list_.set_scale(window_.ui_scale());
        imgui_->begin_frame(input_);
        gizmo_overlay();
        const double now = window_.time_seconds();
        take_dropped_files(now);
        const double dt = std::clamp(now - last_time_, 0.0, 0.1);
        last_time_ = now;
        ctx_.set_theme(studio_theme(night_));
        ctx_.new_frame(now);

        take_results(now);
        place_pending(now);
        advance_playback(dt);
        lose_device_if_asked(now);

        const float width = static_cast<float>(window_.width());
        const float height = static_cast<float>(window_.height());
        const gui::Rect window{0.0F, 0.0F, width, height};
        const gui::Theme& theme = ctx_.theme();
        ctx_.panel(window, theme.background);

        // An open popover owns the pointer where its card is.
        PopoverShield shield{ctx_, scene_view_.covers(ctx_.pointer_x(), ctx_.pointer_y())};

        draw_top_bar(gui::Rect{0.0F, 0.0F, width, top_bar_height});
        const gui::Rect body{10.0F, top_bar_height, std::max(width - 20.0F, 10.0F), std::max(height - top_bar_height - 10.0F, 10.0F)};
        // The viewport owns the center; tree and inspector are workspace
        // panels with independent resize handles.
        const gui::Split left = ctx_.split(gui::ui_id("scene.tree.split"), body, scene_tree_split_, gui::Axis::Row, 8.0F);
        const gui::Split right = ctx_.split(gui::ui_id("scene.inspector.split"), left.second, scene_inspector_split_, gui::Axis::Row, 8.0F);
        const float px = window_.ui_scale();
        scene_view_.tree_panel(document_, ImVec2{left.first.x * px, left.first.y * px},
                               ImVec2{left.first.width * px, left.first.height * px});
        scene_view_.inspector_panel(document_,
                                    compiled_.error.empty() ? std::string_view{build_error_} : std::string_view{compiled_.error},
                                    ImVec2{right.second.x * px, right.second.y * px},
                                    ImVec2{right.second.width * px, right.second.height * px});
        draw_preview(right.first, now);
        stage_area_ = right.first;
        shield.release();
        scene_view_.draw_overlays(ctx_, window, document_);
        draw_toast(window, now);
        ctx_.draw_overlays(window);
        draw_stage_panel_window();
        imgui_->end_frame(list_);

        // Whatever drew edited the scene in place; this is the one look at it
        // each frame, and what follows asks the number it leaves.
        // A motion edit keeps the playhead and playback: the new package
        // simulates again from the initial setup up to the playhead.
        changes_.observe(document_);
        shortcuts();
        record_history();
        follow_document(now);

        window_.set_cursor(ctx_.cursor());
        if (!window_.present(list_, rhi::Color{theme.background.r, theme.background.g, theme.background.b, 1.0F})) {
            // A present can fail for a moment (a minimised window); a lost
            // device never recovers, and the window would only keep its last
            // picture.
            if (const std::string_view lost = window_.device().lost_reason(); !lost.empty()) {
                device_lost(lost);
                // Whatever was saved or not, the scene comes back next time.
                keep_session(false);
                return false;
            }
        }
        return true;
    }

private:
    // ------------------------------------------------------- documents

    // Compiles the page for the preview, as it stands at `at`, here and now:
    // for a page just opened, which has nothing to show until it has. Every
    // edit after that compiles on the compile worker (follow_document), and
    // the fence keeps an older compile still under way there from replacing
    // this one.
    void recompile(std::uint64_t at) {
        compiled_ = compile(document_, Purpose::Preview);
        compiled_for_.set(document_, at);
        natural_loop_ = natural_loop_seconds(compiled_);
        ++compiled_serial_;
        compiler_.fence(at);
        compile_asked_ = at;
    }

    // Takes up the compile worker's newest compile, if it has one newer than
    // the one shown.
    void take_compile() {
        if (std::optional<CompileWorker::Result> done = compiler_.take()) {
            compiled_ = std::move(done->compiled);
            natural_loop_ = done->natural_loop;
            compiled_for_.set(done->document, done->revision);
            ++compiled_serial_;
            // What the baked layers' cache holds is read against it.
            bake_status_dirty_ = true;
        }
    }

    void replace_document(Document document, std::filesystem::path path) {
        document_ = std::move(document);
        path_ = std::move(path);
        const std::uint64_t at = changes_.touch(document_);
        saved_.set(document_, at);
        history_.assign(1U, document_);
        history_index_ = 0U;
        history_head_.set(document_, at);
        scene_view_.forget();
        recompile(at);
        analysis_.reset();
        has_package_ = false;
        // A bake is of the page it was taken from.
        builder_.cancel_bake();
        bake_.reset();
        bake_status_dirty_ = true;
        ++page_generation_;
        want_frame_ = true;
        frame_ = 0.0;
        playing_ = false;
        looping_ = false;
        submit(window_.time_seconds());
        // A CPU run of the scene this one replaced may still finish.
        analysis_from_ = revision_;
    }

    bool open_path(const std::filesystem::path& path) {
        std::string error;
        const std::optional<std::vector<std::uint8_t>> bytes = read_file(path, error);
        if (!bytes) {
            toast(error, false);
            return false;
        }
        Loaded loaded = load_document(std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()});
        if (!loaded.document) {
            toast(path.filename().string() + ": " + loaded.error, false);
            return false;
        }
        replace_document(std::move(*loaded.document), path);
        toast("Opened " + path.filename().string(), true);
        return true;
    }

    // The scene Studio was left on, with its unsaved changes, the camera and
    // the playhead. False when there is none to take up.
    bool resume_page() {
        std::optional<Document> unsaved;
        if (!session_.unsaved.empty()) {
            Loaded loaded = load_document(session_.unsaved);
            unsaved = std::move(loaded.document);
        }
        const std::filesystem::path page{session_.page};
        bool opened = false;
        if (!page.empty()) {
            std::string error;
            if (const std::optional<std::vector<std::uint8_t>> bytes = read_file(page, error)) {
                Loaded loaded =
                    load_document(std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()});
                if (loaded.document) {
                    replace_document(std::move(*loaded.document), page);
                    opened = true;
                }
            }
        }
        if (unsaved) {
            if (opened) {
                // On top of the file, so the scene shows it has changes to save.
                document_ = std::move(*unsaved);
                history_.push_back(document_);
                history_index_ = history_.size() - 1U;
                history_head_.set(document_, changes_.touch(document_));
            } else {
                replace_document(std::move(*unsaved), {});
            }
        } else if (!opened) {
            return false;
        }
        if (session_.camera) {
            const std::array<float, 6>& c = *session_.camera;
            orbit_ = Orbit{c[0], c[1], c[2], mediaserver::scene::Vec3{c[3], c[4], c[5]}};
            want_frame_ = false;
        }
        frame_ = session_.seconds * 60.0;
        playing_ = session_.playing;
        if (!session_.page.empty() && !opened) {
            toast(std::filesystem::path{session_.page}.filename().string() +
                      " could not be opened; its unsaved changes are back",
                  false);
        }
        return true;
    }

    // Writes where Studio is, for next time. `discarded`: the scene had
    // changes the artist chose to throw away, so they are not kept.
    void keep_session(bool discarded) {
        const std::filesystem::path file = session_path();
        if (file.empty()) {
            return;
        }
        SessionState session = session_;
        session.page = path_.empty() ? std::string{} : path_.generic_string();
        session.unsaved.clear();
        if (!discarded && (path_.empty() || unsaved())) {
            session.unsaved = save_document(document_);
        }
        // Window creation applies display scale; zoom is restored separately.
        session.width = static_cast<std::uint32_t>(std::lround(static_cast<float>(window_.width()) * window_.zoom()));
        session.height = static_cast<std::uint32_t>(std::lround(static_cast<float>(window_.height()) * window_.zoom()));
        session.zoom = window_.zoom();
        session.guides = show_guides_;
        session.handles = show_handles_;
        session.paths = show_paths_;
        session.figure = figure_;
        session.night = night_;
        session.looping = looping_;
        session.scene_tree_split = scene_tree_split_;
        session.scene_inspector_split = scene_inspector_split_;
        session.scene_selection = scene_view_.selection();
        session.scene_folded = scene_view_.folded();
        session.camera = std::array<float, 6>{orbit_.yaw,      orbit_.pitch,    orbit_.distance,
                                              orbit_.target.x, orbit_.target.y, orbit_.target.z};
        session.seconds = frame_ / 60.0;
        session.playing = playing_;
        std::error_code ignored;
        std::filesystem::create_directories(file.parent_path(), ignored);
        const std::string text = save_session(session);
        std::string error;
        static_cast<void>(
            write_file(file, std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, error));
    }

    // The GPU device is gone and nothing can be drawn again. Says why in a
    // window of the system's own, which still works, offers to save the scene,
    // and lets Studio close.
    void device_lost(std::string_view reason) {
        std::string message = "ajfx Studio can no longer draw: " + std::string{reason} + ".\n\n";
        if (!unsaved()) {
            message += "The scene has no unsaved changes. Studio will close; open it again to carry on.";
            tinyfd_messageBox("ajfx Studio", message.c_str(), "ok", "error", 1);
            return;
        }
        message += "Save the scene before Studio closes?";
        if (tinyfd_messageBox("ajfx Studio", message.c_str(), "yesno", "error", 1) == 1) {
            static_cast<void>(save(false));
        }
    }

    // --lose-device: has D3D12 remove this window's own device, as a GPU
    // fault or driver reset would, to check what Studio does then.
    void lose_device_if_asked(double now) {
        if (lose_device_at_ < 0.0 || now < lose_device_at_) {
            return;
        }
        lose_device_at_ = -1.0;
#if defined(_WIN32)
        const rhi::NativeHandle native = window_.device().native_handle();
        if (native.backend != rhi::BackendKind::D3d12 || native.value == nullptr) {
            return;
        }
        Microsoft::WRL::ComPtr<ID3D12Device5> device;
        if (SUCCEEDED(static_cast<ID3D12Device*>(native.value)->QueryInterface(IID_PPV_ARGS(&device)))) {
            device->RemoveDevice();
        }
#endif
    }

    // Whether the scene has changes the file does not. Looked at afresh,
    // since the callers are about to keep or discard the scene.
    bool unsaved() {
        return saved_.differs(document_, changes_.observe(document_));
    }

    bool confirm_discard(const char* action) {
        if (!unsaved()) {
            return true;
        }
        const std::string message = "This scene has changes that are not saved. Discard them and " + std::string{action} + "?";
        return tinyfd_messageBox("ajfx Studio", message.c_str(), "yesno", "question", 0) == 1;
    }

    void open_dialog() {
        if (!confirm_discard("open another scene")) {
            return;
        }
        const char* filters[] = {"*.ajfxstudio"};
        if (const char* chosen = tinyfd_openFileDialog("Open an ajfx Studio scene", "", 1, filters, "ajfx Studio scenes", 0)) {
            static_cast<void>(open_path(chosen));
        }
    }

    bool save(bool choose) {
        std::filesystem::path path = path_;
        if (choose || path.empty()) {
            const char* filters[] = {"*.ajfxstudio"};
            const std::string suggestion = (document_.name.empty() ? std::string{"scene"} : document_.name) + ".ajfxstudio";
            const char* chosen = tinyfd_saveFileDialog("Save the scene", suggestion.c_str(), 1, filters, "ajfx Studio scenes");
            if (chosen == nullptr) {
                return false;
            }
            path = chosen;
            if (path.extension() != document_extension) {
                path += document_extension;
            }
        }
        // Models are saved relative to the scene's folder, so the folder can
        // move or be copied and still find them. Saved somewhere new, what
        // the scene holds is first resolved from where it was.
        if (!path_.empty() && path.parent_path() != path_.parent_path()) {
            rebase_models(document_, path_.parent_path(), {});
        }
        Document saving = document_;
        rebase_models(saving, path.parent_path(), path.parent_path());
        const std::string text = save_document(saving);
        std::string error;
        if (!write_file(path, std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, error)) {
            toast(error, false);
            return false;
        }
        path_ = path;
        saved_.set(document_, changes_.observe(document_));
        toast("Saved " + path.filename().string(), true);
        return true;
    }

    void export_dialog() {
        const char* filters[] = {"*.ajfx"};
        std::filesystem::path suggestion = path_.empty() ? std::filesystem::path{document_.name.empty() ? "effect" : document_.name}
                                                         : path_;
        suggestion.replace_extension(".ajfx");
        const char* chosen = tinyfd_saveFileDialog("Export for the media server", suggestion.string().c_str(), 1, filters,
                                                   "ajfx effect packages");
        if (chosen == nullptr) {
            return;
        }
        std::filesystem::path path = chosen;
        if (path.extension() != ".ajfx") {
            path += ".ajfx";
        }
        // The cache goes with it only while it holds the scene as it is: the
        // export checks the rest as a host will.
        builder_.submit_export(document_, path, path_.parent_path(), bake_status_.current ? bake_ : nullptr);
        toast("Exporting " + path.filename().string() + "...", true);
    }

    // Bakes the scene's simulation, or stops the bake under way.
    void bake_or_cancel() {
        if (builder_.bake_progress() >= 0.0) {
            builder_.cancel_bake();
            return;
        }
        if (!bake_status_.any_baked) {
            toast("Nothing to bake: the scene has no objects or particle systems", false);
            return;
        }
        builder_.submit_bake(document_, path_.parent_path());
        bake_asked_for_ = page_generation_;
        bake_quiet_ = false;
    }

    // What baking the scene now would come to, in a sentence.
    [[nodiscard]] std::string bake_cost_text() const {
        const BakeCost& cost = bake_status_.cost;
        std::string text = "about " + bytes_text(cost.bytes) + " over " +
                           format_number(static_cast<float>(cost.frames) / 60.0F, 1) + " s, reading up to " +
                           bytes_text(cost.bytes_per_second) + " a second";
        if (cost.seconds >= 0.0) {
            text += "; baking takes about " + format_number(static_cast<float>(cost.seconds), 1) + " s";
        }
        return text;
    }

    void toast(std::string text, bool ok) {
        toast_ = Toast{std::move(text), ok, window_.time_seconds() + (ok ? 4.0 : 8.0)};
    }

    // ------------------------------------------------------- the builder

    // The selected particle system: the one whose Motion values a package
    // reads live, and whose paths are traced first.
    [[nodiscard]] Uid selected_system() const {
        const Uid selected = scene_view_.selected();
        const SceneItem* item = document_.scene ? document_.scene->item(selected) : nullptr;
        return item != nullptr && std::holds_alternative<ParticleSystem>(item->data) ? selected : 0U;
    }

    void submit(double now) {
        ++revision_;
        const Uid system = selected_system();
        builder_.submit(document_, revision_, path_.parent_path(), system, system);
        submitted_.set(document_, changes_.value());
        submitted_live_ = system;
        ++submit_serial_;
        analysed_.set(document_, changes_.value());
        last_submit_ = now;
        last_analysis_ = now;
    }

    // An edit the package already shows through its live values still
    // moves the paths and bounds: only the CPU run is asked for.
    void submit_analysis(double now) {
        ++revision_;
        builder_.submit_analysis(document_, revision_, path_.parent_path(), selected_system());
        analysed_.set(document_, changes_.value());
        last_analysis_ = now;
    }

    // Whether the preview shows the page as it is: its package was built of
    // it, or differs from it only in values it reads live. Worked out once a
    // change.
    [[nodiscard]] bool preview_current(std::uint64_t at) {
        if (!submitted_.differs(document_, at)) {
            return true;
        }
        if (live_checked_at_ != at || live_checked_serial_ != submit_serial_) {
            live_checked_at_ = at;
            live_checked_serial_ = submit_serial_;
            // Drops and deletions are inputs of the running effect, never
            // a new package.
            const Document then = without_performance(submitted_.copy());
            const Document now = without_performance(document_);
            live_only_ = then == now || (submitted_live_ != 0U && same_but_live(then, now, submitted_live_));
        }
        return live_only_;
    }

    void take_results(double now) {
        take_compile();
        if (std::optional<BuildResult> built = builder_.take_build()) {
            build_error_ = built->error;
            asset_issue_ = built->asset_issue;
            for (auto& [path, outline] : built->outlines) {
                outlines_[path] = std::move(outline);
            }
            if (built->written) {
                package_live_ = built->live;
                // Where the new package takes acts; the performance is
                // turned into actions for it before its first frame.
                package_targets_ = act_targets(built->compiled);
                ++package_generation_;
                settled_frames_ = 0U;
                has_package_ = true;
                kernel_ms_ = built->kernel_ms;
            } else if (!built->compiled.effect && built->compiled.error.empty()) {
                has_package_ = false;
            }
        }
        std::optional<Analysis> analysis = builder_.take_analysis();
        if (analysis && analysis->revision >= analysis_from_ && analysis->focus != 0U) {
            // A quick run of one system: its paths, ahead of the whole run.
            if (analysis->traced && analysis_) {
                std::erase_if(analysis_->paths,
                              [&](const ParticlePath& path) { return path.layer == analysis->focus; });
                for (ParticlePath& path : analysis->paths) {
                    analysis_->paths.push_back(std::move(path));
                }
            }
        } else if (analysis && analysis->revision >= analysis_from_) {
            analysis_ = std::move(analysis);
            // The estimate reads its peaks.
            bake_status_dirty_ = true;
            if (want_frame_) {
                orbit_ = frame_bounds(orbit_, frame_selection_bounds(), preview_aspect_);
                want_frame_ = false;
            }
        }
        if (std::optional<ExportResult> exported = builder_.take_export()) {
            toast(exported->message, exported->ok);
        }
        if (std::optional<SettleOutcome> settled = builder_.take_settle()) {
            settling_ = false;
            // Kept only if the scene's performance is the one it began with:
            // anything done meanwhile would be written over.
            if (settled->ok && document_.scene && document_.scene->performance == settled->before) {
                document_.scene->performance = std::move(settled->after);
                stage_status_ = settled->message;
            } else if (settled->ok) {
                stage_status_ = "The scene changed while it settled: settle again";
            } else {
                stage_status_ = settled->message;
            }
        }
        if (std::optional<BakeResult> baked = builder_.take_bake()) {
            // A bake Studio took by itself says nothing unless it failed for
            // a reason of its own; one an edit stopped is simply taken again.
            if (!bake_quiet_) {
                toast(baked->message, baked->ok);
            } else if (!baked->ok && baked->message != "Bake cancelled" && baked->message != auto_bake_failure_) {
                // Said once, not again each time the page settles.
                toast(baked->message, false);
                auto_bake_failure_ = baked->message;
            }
            // A bake of a scene since replaced is not this scene's.
            if (baked->record && bake_asked_for_ == page_generation_) {
                bake_ = baked->record;
                bake_status_dirty_ = true;
            }
        }
        static_cast<void>(now);
    }

    void follow_document(double now) {
        const std::uint64_t at = changes_.value();
        // An edit is compiled off this thread: on every frame of a drag the
        // page changes, and the compile is too slow to pay for each one here.
        // It is asked for once a revision; the newest wins on the worker.
        const bool compile_behind = compiled_for_.differs(document_, at);
        if (compile_behind && compile_asked_ != at) {
            compiler_.submit(document_, at);
            compile_asked_ = at;
        }
        // What the cache holds, against the scene as it is: only when either
        // changes, and only once the preview's compile, which is the
        // export's, has caught up with the scene.
        if (!compile_behind && (bake_status_dirty_ || bake_status_for_.differs(document_, at))) {
            bake_status_ = bake_status(document_, compiled_, bake_.get(), analysis_ ? &analysis_->peak_alive : nullptr);
            // Studio bakes the solid world by itself once the scene settles.
            bake_status_.automatic = true;
            bake_status_for_.set(document_, at);
            ++bake_serial_;
            bake_status_dirty_ = false;
        }
        const bool current = preview_current(at);
        preview_shows_page_ = current;
        bake_when_settled(now, at, compile_behind);
        if (preview_ && !preview_->status().waiting && has_package_) {
            ++settled_frames_;
        }
        const bool held = scene_view_.gesture_live() || ctx_.pointer_down() || overlay_.dragging();
        if (current && analysed_.differs(document_, at) && now - last_analysis_ >= submit_interval) {
            submit_analysis(now);
        }
        // A newly selected system is made live once nothing is held, so the
        // next drag of one of its values needs no package.
        const Uid system = selected_system();
        if (current && !held && system != 0U && system != submitted_live_ && !builder_.busy()) {
            submit(now);
        }
        if (!current) {
            // While a value is dragged, the next package waits for the preview
            // to have taken in the last; the gesture's end always sends one.
            const bool settled = !builder_.busy() && (settled_frames_ >= settle_frames || !preview_ ||
                                                      now - last_submit_ >= settle_timeout);
            if (!held || (now - last_submit_ >= submit_interval && settled)) {
                submit(now);
            }
        }
    }

    // Bakes a scene with a solid world by itself once it has settled: nothing
    // changed for a moment, nothing held, the preview built and the cache not
    // current. An edit stops a bake Studio took by itself, so a drag never
    // waits on one, and it is taken again once the scene settles again. Once
    // a revision, so a bake that fails is not tried again until the scene
    // changes.
    void bake_when_settled(double now, std::uint64_t at, bool compile_behind) {
        if (at != change_seen_) {
            change_seen_ = at;
            change_time_ = now;
            if (bake_quiet_ && builder_.bake_progress() >= 0.0) {
                builder_.cancel_bake();
            }
        }
        const bool held = scene_view_.gesture_live() || ctx_.pointer_down() || overlay_.dragging();
        const bool settled = !compile_behind && !bake_status_dirty_ && !bake_status_for_.differs(document_, at) &&
                             now - change_time_ >= auto_bake_delay && !held && !builder_.busy() &&
                             preview_shows_page_ && builder_.bake_progress() < 0.0;
        if (!settled || auto_baked_for_ == at || compiled_.solids.emitter.empty() || !bake_status_.any_baked ||
            bake_status_.current) {
            return;
        }
        const bool too_large = std::any_of(bake_status_.layers.begin(), bake_status_.layers.end(),
                                           [](const LayerBake& layer) { return layer.state == BakeState::TooLarge; });
        if (too_large) {
            return;
        }
        builder_.submit_bake(document_, path_.parent_path());
        bake_asked_for_ = page_generation_;
        bake_quiet_ = true;
        auto_baked_for_ = at;
    }

    void record_history() {
        const bool gesture = scene_view_.gesture_live() || ctx_.pointer_down() || ctx_.focus() != gui::UiId{} ||
                             ctx_.pointer_blocked();
        if (gesture || !history_head_.differs(document_, changes_.value())) {
            return;
        }
        history_.resize(history_index_ + 1U);
        history_.push_back(document_);
        if (history_.size() > history_limit) {
            history_.erase(history_.begin());
        }
        history_index_ = history_.size() - 1U;
        history_head_.set(document_, changes_.value());
    }

    // The scene as the history kept it at `index`.
    void step_history(std::size_t index) {
        history_index_ = index;
        document_ = history_[index];
        history_head_.set(document_, changes_.touch(document_));
    }

    void undo() {
        if (history_index_ > 0U) {
            step_history(history_index_ - 1U);
        }
    }

    void redo() {
        if (history_index_ + 1U < history_.size()) {
            step_history(history_index_ + 1U);
        }
    }

    void shortcuts() {
        if (ctx_.key_pressed(gui::Key::ZoomIn)) {
            window_.set_zoom(std::min(window_.zoom() + 0.25F, 1.5F));
        } else if (ctx_.key_pressed(gui::Key::ZoomOut)) {
            window_.set_zoom(std::max(window_.zoom() - 0.25F, 0.75F));
        } else if (ctx_.key_pressed(gui::Key::ZoomReset)) {
            window_.set_zoom(1.0F);
        }
        if (ctx_.key_pressed(gui::Key::Save)) {
            static_cast<void>(save(false));
        }
        if (ctx_.typing()) {
            return;
        }
        if (ctx_.key_pressed(gui::Key::Undo)) {
            undo();
        } else if (ctx_.key_pressed(gui::Key::Redo)) {
            redo();
        }
        // Letters and the space bar act only with the pointer over the
        // viewport and timeline. Typed over the tree or the inspector they
        // were meant for a field, and a click that missed it must not turn
        // a name into a reframed camera and looping playback.
        const bool over_stage = stage_area_.contains(ctx_.pointer_x(), ctx_.pointer_y());
        const std::string_view typed = over_stage ? ctx_.shortcut_text() : std::string_view{};
        if (typed.find(' ') != std::string_view::npos) {
            toggle_playing();
        }
        if (typed.find('l') != std::string_view::npos || typed.find('L') != std::string_view::npos) {
            looping_ = !looping_;
        }
        if (typed.find('/') != std::string_view::npos) {
            scene_view_.open_add(20.0F, top_bar_height + 40.0F);
        }
        if (typed.find('f') != std::string_view::npos || typed.find('F') != std::string_view::npos) {
            orbit_ = frame_bounds(orbit_, frame_selection_bounds(), preview_aspect_);
        }
        if (typed.find('g') != std::string_view::npos || typed.find('G') != std::string_view::npos) {
            show_guides_ = !show_guides_;
        }
        if (typed.find('h') != std::string_view::npos || typed.find('H') != std::string_view::npos) {
            show_handles_ = !show_handles_;
        }
        if (typed.find('p') != std::string_view::npos || typed.find('P') != std::string_view::npos) {
            show_paths_ = !show_paths_;
        }
        if (typed.find('d') != std::string_view::npos || typed.find('D') != std::string_view::npos) {
            tool_ = tool_ == StageTool::Drop ? StageTool::Select : StageTool::Drop;
        }
        if (typed.find('o') != std::string_view::npos || typed.find('O') != std::string_view::npos) {
            tool_ = tool_ == StageTool::Place ? StageTool::Select : StageTool::Place;
        }
        if (typed.find('x') != std::string_view::npos || typed.find('X') != std::string_view::npos) {
            tool_ = tool_ == StageTool::Delete ? StageTool::Select : StageTool::Delete;
        }
        if (over_stage && ctx_.key_pressed(gui::Key::Escape)) {
            tool_ = StageTool::Select;
        }
        // The act picked on the timeline goes with Delete.
        if (over_stage && selected_act_ != 0U &&
            (ctx_.key_pressed(gui::Key::Delete) || ctx_.key_pressed(gui::Key::Backspace))) {
            if (remove_act(document_.scene->performance, selected_act_)) {
                toast("Taken away: the scene replays without it", true);
            }
            selected_act_ = 0U;
        }
        if (ctx_.key_pressed(gui::Key::Home)) {
            frame_ = 0.0;
        }
        if (ctx_.key_pressed(gui::Key::Left)) {
            playing_ = false;
            frame_ = std::max(std::floor(frame_) - 1.0, 0.0);
        }
        if (ctx_.key_pressed(gui::Key::Right)) {
            playing_ = false;
            frame_ = std::fmod(std::floor(frame_) + 1.0, loop_frames());
        }
    }

    // ------------------------------------------------------- playback

    [[nodiscard]] double loop_frames() const {
        return std::max(std::round(static_cast<double>(loop_seconds(document_, compiled_)) * 60.0), 30.0);
    }

    void advance_playback(double dt) {
        const double frames = loop_frames();
        if (playing_) {
            frame_ += dt * 60.0;
            // A scene that does not loop plays on past its capture range.
            if (looping_ && frame_ >= frames) {
                frame_ = std::fmod(frame_, frames);
            }
        }
        // Short of the loop's length rather than its last whole frame: on a
        // display faster than 60 Hz a refresh advances less than a frame, and
        // the playhead still has to reach the end to wrap.
        frame_ = looping_ ? std::clamp(frame_, 0.0, std::nextafter(frames, 0.0)) : std::max(frame_, 0.0);
    }

    void toggle_playing() {
        playing_ = !playing_;
    }

    // ------------------------------------------------------- top bar

    // The bar across the top, in Dear ImGui: the file actions, the bake, the
    // scene's file and whether it changed, history, night, and the state of
    // the preview as a dot and a word.
    void draw_top_bar(const gui::Rect& bar) {
        const float scale = window_.ui_scale();
        const ImVec4 good{0.36F, 0.80F, 0.52F, 1.0F};
        const ImVec4 caution{1.0F, 0.72F, 0.25F, 1.0F};
        const ImVec4 warning{0.95F, 0.40F, 0.35F, 1.0F};
        ImGui::SetNextWindowPos(ImVec2{bar.x * scale, bar.y * scale});
        ImGui::SetNextWindowSize(ImVec2{bar.width * scale, bar.height * scale});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{18.0F * scale, 8.0F * scale});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                           ImGuiWindowFlags_NoScrollWithMouse;
        if (!ImGui::Begin("##topbar", nullptr, flags)) {
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            return;
        }
        const bool compact = bar.width < 1100.0F;
        const auto tip = [](const std::string& text) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", text.c_str());
            }
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(compact ? "ajfx" : "ajfx Studio");
        ImGui::SameLine();
        if (ImGui::Button("New")) {
            if (confirm_discard("start a new scene")) { replace_document(new_scene(), {}); }
        }
        tip("Start a new scene with a Ground object");
        ImGui::SameLine();
        if (ImGui::Button("Open")) {
            open_dialog();
        }
        tip("Open an ajfx Studio scene");
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            static_cast<void>(save(false));
        }
        tip("Save the scene (Ctrl+S)");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("Export .ajfx")) {
            export_dialog();
        }
        ImGui::PopStyleColor();
        tip("Export a package the media server plays, kernels included");

        const double progress = builder_.bake_progress();
        const bool baking = progress >= 0.0;
        std::string label = "Bake";
        std::string baking_tip;
        if (baking) {
            label = "Baking " + std::to_string(static_cast<int>(std::lround(progress * 100.0))) + "%";
            baking_tip = "Working out the simulation on the CPU. Click to stop.";
        } else if (!bake_status_.any_baked) {
            const bool smoke_only = std::any_of(bake_status_.layers.begin(), bake_status_.layers.end(),
                                                [](const LayerBake& layer) { return layer.state == BakeState::Live && !layer.why.empty(); });
            baking_tip = smoke_only ? "Nothing to cache: a system feeding smoke stays live, since a cache keeps particles, not what they deposit."
                                    : "Add an object or a particle system first.";
        } else if (bake_status_.current) {
            baking_tip = "Cache is up to date. Baking again: " + bake_cost_text() + ".";
        } else {
            baking_tip = "Cache the simulation: " + bake_cost_text() + ".";
        }
        ImGui::SameLine();
        if (bake_status_.any_baked && !baking) {
            label += "   ";
        }
        const ImVec2 bake_at = ImGui::GetCursorScreenPos();
        if (ImGui::Button(label.c_str())) {
            bake_or_cancel();
        }
        const ImVec2 bake_size = ImGui::GetItemRectSize();
        tip(baking_tip);
        if (bake_status_.any_baked && !baking) {
            // A dot on the button: green when the cache holds the scene,
            // amber when it needs taking (again).
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2{bake_at.x + bake_size.x - 10.0F * scale, bake_at.y + bake_size.y * 0.5F}, 3.5F * scale,
                ImGui::ColorConvertFloat4ToU32(bake_status_.current ? good : caution));
        }

        // The scene's file, and whether it has unsaved changes.
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        const std::string file = path_.empty() ? std::string{"Not saved yet"} : path_.filename().string();
        ImGui::TextDisabled("%s", file.c_str());
        if (saved_.differs(document_, changes_.value())) {
            ImGui::SameLine(0.0F, 6.0F * scale);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2{at.x + 3.0F * scale, at.y + ImGui::GetFrameHeight() * 0.5F},
                                                        3.0F * scale, ImGui::ColorConvertFloat4ToU32(caution));
            ImGui::Dummy(ImVec2{6.0F * scale, 1.0F});
            tip("Changed since it was saved");
        }

        // The state of the effect, as a dot and a word.
        std::string state = "Up to date";
        ImVec4 tone = good;
        std::string state_tip = "The preview shows this scene.";
        if (!preview_error_.empty()) {
            state = "No preview";
            tone = warning;
            state_tip = preview_error_;
        } else if (!build_error_.empty()) {
            state = "Cannot build";
            tone = warning;
            state_tip = build_error_;
        } else if (builder_.busy() || !preview_shows_page_) {
            state = "Updating";
            tone = caution;
            state_tip = "Building the preview from your latest change.";
        } else if (!compiled_.error.empty()) {
            state = "Cannot play";
            tone = warning;
            state_tip = compiled_.error;
        } else if (!asset_issue_.empty()) {
            state = "A file cannot be used";
            tone = caution;
            state_tip = asset_issue_ + ". Export waits for it.";
        } else if (kernel_ms_ > 0.0) {
            state_tip += " Its kernels compiled in " + format_number(static_cast<float>(kernel_ms_), 0) + " ms.";
        }
        const std::string shown = compact ? (state == "Up to date" ? "Ready" : state == "Updating" ? "Wait" : "Issue") : state;
        const float chip_w = ImGui::CalcTextSize(shown.c_str()).x + 28.0F * scale;
        const float buttons_w = ImGui::CalcTextSize("Undo").x + ImGui::CalcTextSize("Redo").x + ImGui::CalcTextSize("Night").x +
                                ImGui::GetStyle().FramePadding.x * 6.0F + ImGui::GetStyle().ItemSpacing.x * 3.0F;
        ImGui::SameLine(bar.width * scale - 18.0F * scale - buttons_w - chip_w - ImGui::GetStyle().ItemSpacing.x);
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetFrameHeight();
            ImDrawList* list = ImGui::GetWindowDrawList();
            list->AddRectFilled(at, ImVec2{at.x + chip_w, at.y + h}, ImGui::GetColorU32(ImGuiCol_FrameBg), h * 0.5F);
            list->AddCircleFilled(ImVec2{at.x + 12.0F * scale, at.y + h * 0.5F}, 3.5F * scale,
                                  ImGui::ColorConvertFloat4ToU32(tone));
            list->AddText(ImVec2{at.x + 22.0F * scale, at.y + (h - ImGui::GetFontSize()) * 0.5F},
                          ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
            ImGui::Dummy(ImVec2{chip_w, h});
            tip(state + ": " + state_tip);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(history_index_ == 0U);
        if (ImGui::Button("Undo")) {
            undo();
        }
        ImGui::EndDisabled();
        tip("Undo (Ctrl+Z)");
        ImGui::SameLine();
        ImGui::BeginDisabled(history_index_ + 1U >= history_.size());
        if (ImGui::Button("Redo")) {
            redo();
        }
        ImGui::EndDisabled();
        tip("Redo (Ctrl+Y)");
        ImGui::SameLine();
        if (ImGui::Button(night_ ? "Day" : "Night")) {
            night_ = !night_;
        }
        tip(night_ ? "Turn off night dim" : "Dim the workspace for night");
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    // What was just done or refused, over the bottom of the window for a few
    // seconds.
    void draw_toast(const gui::Rect& window, double now) {
        if (toast_.text.empty() || now > toast_.until) {
            return;
        }
        const float scale = window_.ui_scale();
        ImDrawList* list = ImGui::GetForegroundDrawList();
        const ImVec2 text = ImGui::CalcTextSize(toast_.text.c_str());
        const float w = std::min(text.x + 44.0F * scale, (window.width - 40.0F) * scale);
        const float h = 36.0F * scale;
        const ImVec2 min{window.center_x() * scale - w * 0.5F, (window.bottom() - 70.0F) * scale};
        list->AddRectFilled(ImVec2{min.x + 2.0F, min.y + 4.0F}, ImVec2{min.x + w + 2.0F, min.y + h + 4.0F},
                            IM_COL32(0, 0, 0, 80), h * 0.5F);
        list->AddRectFilled(min, ImVec2{min.x + w, min.y + h}, ImGui::GetColorU32(ImGuiCol_PopupBg), h * 0.5F);
        list->AddCircleFilled(ImVec2{min.x + 17.0F * scale, min.y + h * 0.5F}, 3.5F * scale,
                              toast_.ok ? IM_COL32(92, 204, 133, 255) : IM_COL32(242, 102, 89, 255));
        list->AddText(ImVec2{min.x + 28.0F * scale, min.y + (h - text.y) * 0.5F}, ImGui::GetColorU32(ImGuiCol_Text),
                      toast_.text.c_str());
    }

    // ------------------------------------------------------- preview

    Bounds frame_selection_bounds() const {
        Bounds bounds;
        for (const SceneItem& item : document_.scene->items) {
            if (!scene_view_.selection().empty() && std::find(scene_view_.selection().begin(), scene_view_.selection().end(), item.uid) == scene_view_.selection().end()) { continue; }
            Vec3 at;
            float radius{};
            if (const auto* object = std::get_if<Object>(&item.data)) {
                if (object->mesh == "floor" && scene_view_.selection().empty()) { continue; }
                at = object->position;
                radius = object->size;
            } else if (const auto* system = std::get_if<ParticleSystem>(&item.data)) {
                at = system->position;
                radius = std::max(system->radius.max, 0.1F);
            } else if (const auto* force = std::get_if<Force>(&item.data)) {
                if (force->kind == ForceKind::Wind) { continue; }
                at = force->position;
                radius = std::max({force->kind == ForceKind::Blast ? force->radius : force->reach, 0.5F});
            } else if (const auto* grid = std::get_if<SmokeGrid>(&item.data)) {
                at = grid->position;
                radius = std::max({grid->size.x, grid->size.y, grid->size.z, 0.2F});
            } else { continue; }
            const Vec3 r{radius, radius, radius};
            const Vec3 low = (at - r) * document_.scale;
            const Vec3 high = (at + r) * document_.scale;
            if (!bounds.valid) { bounds = Bounds{low, high, true}; }
            else {
                bounds.minimum = {std::min(bounds.minimum.x, low.x), std::min(bounds.minimum.y, low.y), std::min(bounds.minimum.z, low.z)};
                bounds.maximum = {std::max(bounds.maximum.x, high.x), std::max(bounds.maximum.y, high.y), std::max(bounds.maximum.z, high.z)};
            }
        }
        return bounds;
    }

    void draw_preview(const gui::Rect& pane, double now) {
        const gui::Theme& theme = ctx_.theme();
        ctx_.rounded(pane, theme.panel, theme.radius_large);
        const std::vector<Track> tracks = collect_tracks(*document_.scene, loop_seconds(document_, compiled_));
        const float timeline_h =
            tracks.empty() ? 0.0F : track_height * static_cast<float>(std::min(tracks.size(), tracks_shown)) + 10.0F;
        const gui::Rect view{pane.x + 8.0F, pane.y + 8.0F, pane.width - 16.0F,
                             std::max(pane.height - transport_height - timeline_h - 16.0F, 10.0F)};
        preview_aspect_ = view.width / std::max(view.height, 1.0F);

        const rhi::Texture* picture = render_preview(view);
        if (picture != nullptr) {
            ctx_.image(view, *picture);
        } else {
            ctx_.rounded(view, theme.track, theme.radius);
        }
        // Guides, handles and paths over the picture. Their handles take a
        // press before the camera does.
        const Uid working = scene_view_.selected();
        if (scene_guides_for_ != changes_.value()) {
            scene_guides_ = lower_scene(document_);
            scene_guides_for_ = changes_.value();
        }
        // A force's guides are drawn in the system it acts on: its handle's layer.
        const SceneItem* working_item = document_.scene->item(working);
        const bool force_selected = working_item && std::holds_alternative<Force>(working_item->data);
        GuideOptions guide_options{.focus = handle_block(document_, working),
                                   .layer = force_selected ? 0U : working,
                                   .seconds = static_cast<float>(frame_ / 60.0),
                                   .outlines = &outlines_};
        if (analysis_ && analysis_->bounds.valid) {
            guide_options.bounds = std::pair{analysis_->bounds.minimum, analysis_->bounds.maximum};
        }
        // The handles drag the lowered copy; the item takes back what moved.
        // Only while a handle is held: the copy is lowered from the scene as
        // the last frame left it, so taking it back otherwise would undo an
        // edit typed into the inspector this frame.
        const GuideSet guides = build_guides(scene_guides_, guide_options);
        const bool handle_held = overlay_.draw(ctx_, view, orbit_, scene_guides_,
                                               OverlayInputs{.guides = &guides,
                                                             .analysis = analysis_ ? &*analysis_ : nullptr,
                                                             .show_guides = show_guides_,
                                                             .show_handles = show_handles_,
                                                             .show_paths = show_paths_,
                                                             .layer = working,
                                                             .frame = frame_});
        if (handle_held) {
            static_cast<void>(adopt_handle_edit(document_, scene_guides_, working));
        }
        ctx_.draw_list().add_corner_mask(view.x, view.y, view.width, view.height, theme.radius, theme.panel);

        // What the picture is, and anything holding it.
        // A held picture is the last complete one, so what held it is not
        // said until the hold runs out.
        std::string note = "Initial setup";
        gui::UiColor note_tone = theme.text_muted;
        const bool held = preview_ && preview_->status().held;
        if (!preview_error_.empty()) {
            note = preview_error_;
            note_tone = theme.warning;
        } else if (preview_ && !held && !preview_->status().issue.empty()) {
            note = preview_->status().issue;
            note_tone = theme.warning;
        } else if (preview_ && !held && preview_->status().waiting) {
            note = "Loading the effect";
            note_tone = theme.caution;
        } else if (!has_package_ && compiled_.effect) {
            note = build_error_.empty() ? "Building the effect" : build_error_;
            note_tone = build_error_.empty() ? theme.caution : theme.warning;
        } else if (bake_quiet_ && builder_.bake_progress() >= 0.0) {
            // Quiet: it never holds anything up, and an edit stops it.
            note = "Baking... " + std::to_string(static_cast<int>(std::lround(builder_.bake_progress() * 100.0))) + "%";
        } else if (tool_ != StageTool::Select) {
            note = tool_hint();
        } else if (show_paths_ && analysis_ && !analysis_->traced) {
            note = "Too many particles to trace their paths";
            note_tone = theme.caution;
        } else if (show_paths_ && !analysis_) {
            note = "Tracing particle paths";
            note_tone = theme.caution;
        } else if (show_paths_ && working == 0U) {
            note = "Select an item to see its paths";
            note_tone = theme.caution;
        } else if (show_paths_) {
            const auto count = static_cast<std::size_t>(
                std::count_if(analysis_->paths.begin(), analysis_->paths.end(),
                              [&](const ParticlePath& path) { return path.layer == working; }));
            const SceneItem* item = document_.scene->item(working);
            note = "Paths of " + thousands(count) + " particles" +
                   (item != nullptr && !item->name.empty() ? " in " + item->name : std::string{});
        }
        const float note_w = std::min(ctx_.font_width(note) + 24.0F, view.width - 100.0F);
        const gui::Rect note_rect{view.x + 10.0F, view.y + 10.0F, note_w, 24.0F};
        // Nearly opaque: the guides drawn before it must not show through
        // its words.
        ctx_.rounded(note_rect, gui::fade(theme.background, 0.94F), note_rect.height * 0.5F);
        ctx_.label(gui::Rect{note_rect.x + 12.0F, note_rect.y, note_rect.width - 16.0F, note_rect.height}, note, note_tone);
        ctx_.tooltip(note_rect, "Motion edits simulate again from the initial setup up to the playhead. Color edits preserve motion caches. Preview is simulating; cache playback is not connected yet.");

        // View tools, top right.
        const gui::Rect frame_button{view.right() - 38.0F, view.y + 8.0F, 30.0F, 30.0F};
        if (editor::icon_button(ctx_, gui::ui_id("studio.frame"), frame_button, gui::Icon::Fit, "Frame the selection, or the scene (F)")) {
            orbit_ = frame_bounds(orbit_, frame_selection_bounds(), preview_aspect_);
        }
        const gui::Rect figure_button{frame_button.x - 34.0F, frame_button.y, 30.0F, 30.0F};
        if (editor::icon_button(ctx_, gui::ui_id("studio.figure"), figure_button, gui::Icon::Measure,
                                figure_ ? "Hide the 1.8 m figure" : "Show a 1.8 m figure for scale", figure_)) {
            figure_ = !figure_;
        }
        const gui::Rect paths_button{figure_button.x - 34.0F, frame_button.y, 30.0F, 30.0F};
        if (editor::icon_button(ctx_, gui::ui_id("studio.paths"), paths_button, gui::Icon::Paths,
                                show_paths_ ? "Hide particle paths (P)"
                                            : "Show the whole path of some particles at once (P)",
                                show_paths_)) {
            show_paths_ = !show_paths_;
        }
        const gui::Rect handles_button{paths_button.x - 34.0F, frame_button.y, 30.0F, 30.0F};
        if (editor::icon_button(ctx_, gui::ui_id("studio.handles"), handles_button, gui::Icon::Gizmo,
                                show_handles_ ? "Hide the handles that drag the selected item (H)"
                                              : "Show handles to drag the selected item (H)",
                                show_handles_)) {
            show_handles_ = !show_handles_;
        }
        const gui::Rect guides_button{handles_button.x - 34.0F, frame_button.y, 30.0F, 30.0F};
        if (editor::icon_button(ctx_, gui::ui_id("studio.guides"), guides_button, gui::Icon::Guides,
                                show_guides_ ? "Hide guides (G)" : "Show guides (G)",
                                show_guides_)) {
            show_guides_ = !show_guides_;
        }
        // What a click in the picture does: select, drop or delete.
        struct ToolButton final {
            StageTool tool;
            gui::Icon icon;
            const char* tip;
        };
        constexpr std::array<ToolButton, 4> tools{
            ToolButton{StageTool::Delete, gui::Icon::Trash, "Delete: click a thing to take it away from then on (X)"},
            ToolButton{StageTool::Drop, gui::Icon::Surface,
                       "Drop: click to drop a copy of the selected object there, while it plays (D)"},
            ToolButton{StageTool::Place, gui::Icon::Plus,
                       "Place: click to put a copy of the selected object on the stage, standing there from the start (O)"},
            ToolButton{StageTool::Select, gui::Icon::Select, "Select: click a thing to select it (Esc)"}};
        float tool_x = guides_button.x - 12.0F;
        for (const ToolButton& button : tools) {
            tool_x -= 34.0F;
            const gui::Rect at{tool_x, frame_button.y, 30.0F, 30.0F};
            if (editor::icon_button(ctx_, part_id(static_cast<std::uint32_t>(button.tool), "studio.tool"), at,
                                    button.icon, button.tip, tool_ == button.tool)) {
                tool_ = button.tool;
            }
        }
        // After the tools over the picture: the first control a press lands
        // on takes it, and the camera covers the whole view.
        camera_input(view);

        // What it costs, bottom left.
        // The HUD: what the scene costs and how close its limits are, at the
        // foot of the picture. The same lines go to Scene settings.
        const SceneStats stats = scene_stats(*document_.scene, compiled_.effect ? &compiled_ : nullptr,
                                             analysis_ && analysis_->traced ? &analysis_->peak_alive : nullptr);
        std::vector<std::string> hud = stat_lines(stats);
        scene_view_.set_statistics(hud);
        std::string cost = "GPU " + (gpu_ms_ > 0.0 ? format_number(static_cast<float>(gpu_ms_), 2) + " ms a frame" : std::string{"not timed yet"});
        if (bake_status_.any_baked && bake_status_.cached_frames > 0U) {
            cost += bake_status_.current ? ", cache holds " + format_number(static_cast<float>(bake_status_.cached_frames) / 60.0F, 1) + " s"
                                         : ", cache stale";
        } else {
            cost += ", no cache";
        }
        // Systems feeding smoke are never cached: said here, beside the
        // cache, rather than failing the bake.
        const auto smoke_live = static_cast<std::size_t>(std::count_if(
            bake_status_.layers.begin(), bake_status_.layers.end(),
            [](const LayerBake& layer) { return layer.state == BakeState::Live && !layer.why.empty(); }));
        if (smoke_live > 0U) {
            cost += smoke_live == 1U ? ", 1 system feeding smoke stays live"
                                     : ", " + std::to_string(smoke_live) + " systems feeding smoke stay live";
        }
        hud.insert(hud.begin(), cost);
        if (hud.size() > 5U) { hud.resize(5U); }
        float hud_w = 0.0F;
        for (const std::string& line : hud) { hud_w = std::max(hud_w, ctx_.font_width(line) + 24.0F); }
        hud_w = std::min(hud_w, view.width - 20.0F);
        const float line_h = 20.0F;
        const gui::Rect hud_rect{view.x + 10.0F, view.bottom() - 10.0F - line_h * static_cast<float>(hud.size()) - 8.0F, hud_w,
                                 line_h * static_cast<float>(hud.size()) + 8.0F};
        ctx_.rounded(hud_rect, gui::fade(theme.background, 0.94F), 10.0F);
        for (std::size_t k = 0U; k < hud.size(); ++k) {
            const bool warning = hud[k].rfind("Near a limit.", 0U) == 0U;
            ctx_.label(gui::Rect{hud_rect.x + 12.0F, hud_rect.y + 4.0F + line_h * static_cast<float>(k), hud_rect.width - 16.0F, line_h},
                       hud[k], warning ? theme.caution : theme.text_muted);
        }
        ctx_.tooltip(hud_rect, "GPU time for the whole preview frame, then what the scene costs. A limit is named once it is four fifths used.");

        const gui::Rect bar{pane.x + 8.0F, view.bottom() + 8.0F, pane.width - 16.0F, transport_height - 8.0F};
        draw_transport(bar);
        if (!tracks.empty()) {
            draw_timeline(gui::Rect{bar.x, bar.bottom() + 2.0F, bar.width, timeline_h - 2.0F}, tracks, scrub_track(bar));
        }
        static_cast<void>(now);
    }

    void camera_input(const gui::Rect& view) {
        const gui::UiId id = gui::ui_id("studio.preview.camera");
        const gui::Interaction hit = ctx_.grab(id, view);
        const bool over = view.contains(ctx_.pointer_x(), ctx_.pointer_y()) && !ctx_.pointer_blocked();
        if (hit.held && !hit.pressed) {
            orbit_.yaw -= ctx_.pointer_dx() / 700.0F;
            orbit_.pitch = std::clamp(orbit_.pitch + ctx_.pointer_dy() / 700.0F, -0.02F, 0.24F);
            want_frame_ = false;
        }
        stage_view_ = view;
        pick_click(view, hit);
        if (hit.clicked && clicks_.double_click(1U, ctx_.time_seconds())) {
            orbit_ = frame_bounds(orbit_, frame_selection_bounds(), preview_aspect_);
        }
        if (over || panning_ || dollying_) {
            if (ctx_.middle_pointer_pressed() && over) {
                panning_ = true;
            }
            if (ctx_.secondary_pointer_pressed() && over) {
                dollying_ = true;
            }
            if (!ctx_.middle_pointer_down()) {
                panning_ = false;
            }
            if (!ctx_.secondary_pointer_down()) {
                dollying_ = false;
            }
            const float two_pi = 6.28318530718F;
            if (panning_) {
                const float yaw = orbit_.yaw * two_pi;
                const float scale = orbit_.distance / std::max(view.height, 1.0F) * 0.8F;
                const mediaserver::scene::Vec3 right{std::cos(yaw), 0.0F, -std::sin(yaw)};
                orbit_.target = orbit_.target - right * (ctx_.pointer_dx() * scale) +
                                mediaserver::scene::Vec3{0.0F, ctx_.pointer_dy() * scale, 0.0F};
                want_frame_ = false;
            }
            if (dollying_) {
                orbit_.distance = std::clamp(orbit_.distance * std::exp(ctx_.pointer_dy() * 0.005F), 0.2F, 500.0F);
                want_frame_ = false;
            }
            if (over && ctx_.wheel() != 0.0F) {
                orbit_.distance = std::clamp(orbit_.distance * std::exp(-ctx_.wheel() * 0.12F), 0.2F, 500.0F);
                want_frame_ = false;
            }
        }
        if (hit.held || panning_ || dollying_) {
            ctx_.set_cursor(gui::Cursor::Move);
        }
    }

    // A click in the preview selects the solid thing under it, where the
    // analysis's samples have it at the playhead; Ctrl adds to the selection.
    // With Drop, it drops a copy of the selected object above where it lands;
    // with Delete, it takes the thing under it away.
    void pick_click(const gui::Rect& view, const gui::Interaction& hit) {
        if (!hit.clicked) {
            return;
        }
        const StageBodies* standing =
            analysis_ ? stage_bodies_at(analysis_->bodies, static_cast<std::uint64_t>(frame_)) : nullptr;
        const std::span<const StageBody> bodies =
            standing != nullptr ? std::span<const StageBody>{standing->bodies} : std::span<const StageBody>{};
        const StageCamera camera{camera_pose(orbit_), view};
        Vec3 origin{}, direction{};
        camera.ray(ctx_.pointer_x(), ctx_.pointer_y(), origin, direction);
        const std::optional<StageHit> picked = pick_stage(bodies, origin, direction, stage_floor(document_));
        if (tool_ == StageTool::Select) {
            if (picked && picked->on_body) {
                scene_view_.select(picked->layer, ctx_.ctrl_held());
                // A thing put down or dropped is picked as itself, to be
                // moved with the gizmo or timed on the timeline.
                if (const std::optional<BodyRef> ref =
                        picked->spawn_index ? body_ref(package_targets_, *picked->spawn_index) : std::nullopt;
                    ref && ref->drop != 0U) {
                    selected_act_ = ref->drop;
                }
            }
            return;
        }
        if (!picked) {
            toast("That click lands on nothing: aim at the floor or a thing", false);
            return;
        }
        if (!has_package_) {
            toast("Wait for the scene to finish building", false);
            return;
        }
        const std::uint64_t tick =
            tool_ == StageTool::Place ? 0U : act_tick(playing_, shown_tick_, preview_acting_lead);
        if (tool_ != StageTool::Place && looping_ && static_cast<double>(tick) >= loop_frames()) {
            toast("That is past the end of the loop: it would never play", false);
            return;
        }
        const StageAct act = act_on_stage(document_, package_targets_, tool_, scene_view_.selected(), *picked, tick);
        if (act.serial == 0U) {
            toast(act.refusal, false);
            return;
        }
        selected_act_ = act.serial;
        if (!playing_) {
            toast(tool_ == StageTool::Drop    ? "Dropped: it falls once the scene plays on"
                  : tool_ == StageTool::Place ? "Placed: it stands there when the scene starts"
                                              : "Deleted: it goes once the scene plays on",
                  true);
        }
    }

    // The gizmo over the thing picked on the stage: drawn before the rest, so
    // a press on it is not also a press on the picture under it.
    void gizmo_overlay() {
        if (!document_.scene || selected_act_ == 0U || tool_ != StageTool::Select || stage_view_.width <= 1.0F) {
            return;
        }
        const Act* act = find_act(document_.scene->performance, selected_act_);
        if (act == nullptr || !on_stage(*act)) {
            return;
        }
        const float scale = window_.ui_scale();
        const gui::Rect pixels{stage_view_.x * scale, stage_view_.y * scale, stage_view_.width * scale,
                               stage_view_.height * scale};
        const StageCamera camera{camera_pose(orbit_), pixels};
        const GizmoOutcome gizmo = draw_gizmo(camera, pixels, stage_panel_.gizmo, GizmoPose{act->position, act->turn});
        if (gizmo.took_pointer) {
            input_.withhold_pointer();
        }
        if (gizmo.moved) {
            move_placement(document_.scene->performance, selected_act_, gizmo.moved->position, gizmo.moved->turn);
        }
    }

    // Model files let go over the viewport become kinds placed by hand, and
    // the tool becomes Place so more copies are a click each.
    void take_dropped_files(double now) {
        if (!document_.scene || input_.dropped_files().empty()) {
            return;
        }
        const float x = input_.drop_x();
        const float y = input_.drop_y();
        if (!stage_area_.contains(x, y)) {
            return;
        }
        const StageCamera camera{camera_pose(orbit_), stage_area_};
        Vec3 origin{}, direction{};
        camera.ray(x, y, origin, direction);
        const float floor = stage_floor(document_);
        // Where the ray meets the floor, or a little in front of the camera
        // when it never does.
        const Vec3 where = direction.y < -1.0e-4F ? origin + direction * ((floor - origin.y) / direction.y)
                                                   : origin + direction * 3.0F;
        std::size_t added = 0U;
        for (const Uid uid : add_dropped_models(document_, input_.dropped_files())) {
            // Several at once are set side by side rather than in one spot.
            pending_places_.push_back(PendingPlace{uid, where + Vec3{0.6F * static_cast<float>(added), 0.0F, 0.0F}, now});
            scene_view_.select(uid, false);
            ++added;
        }
        if (added != 0U) {
            tool_ = StageTool::Place;
            toast(added == 1U ? "Placed on the stage once it has built: click to place more"
                              : "Placed on the stage once they have built: click to place more",
                  true);
        }
    }

    // Puts the first copy of each dropped model on the stage once the
    // package that has its kind has been built; gives up on one that never
    // is, saying why.
    void place_pending(double now) {
        for (auto at = pending_places_.begin(); at != pending_places_.end();) {
            if (document_.scene && has_package_ && package_targets_.kinds.contains(at->kind)) {
                StageHit hit;
                hit.point = at->at;
                const StageAct act =
                    act_on_stage(document_, package_targets_, StageTool::Place, at->kind, hit, 0U);
                if (act.serial == 0U) {
                    toast(act.refusal, false);
                } else {
                    selected_act_ = act.serial;
                }
                at = pending_places_.erase(at);
            } else if (now - at->since > 30.0 || !document_.scene || document_.scene->item(at->kind) == nullptr) {
                toast("A dropped model could not be placed: its object did not build", false);
                at = pending_places_.erase(at);
            } else {
                ++at;
            }
        }
    }

    // The Stage panel (Dear ImGui), and what its setup buttons ask for.
    void draw_stage_panel_window() {
        if (!document_.scene) {
            return;
        }
        const float scale = window_.ui_scale();
        const StagePanelIn in{.document = &document_,
                              .kind = scene_view_.selected(),
                              .settling = settling_,
                              .status = stage_status_};
        const StageRequest request = draw_stage_panel(
            stage_panel_, in, tool_, ImVec2{(stage_area_.x + 14.0F) * scale, (stage_area_.y + 60.0F) * scale});
        Performance& performance = document_.scene->performance;
        switch (request) {
        case StageRequest::Jitter: {
            const std::size_t moved = jitter_stage(performance, static_cast<std::uint32_t>(stage_panel_.seed),
                                                   stage_panel_.shift, stage_panel_.tilt);
            stage_status_ = "Jittered " + std::to_string(moved) + (moved == 1U ? " thing" : " things");
            break;
        }
        case StageRequest::Reset: {
            const std::size_t moved = reset_stage(performance);
            stage_status_ = "Put " + std::to_string(moved) + (moved == 1U ? " thing" : " things") + " back";
            break;
        }
        case StageRequest::Settle:
            settling_ = true;
            stage_status_.clear();
            builder_.submit_settle(document_, path_.empty() ? std::filesystem::path{} : path_.parent_path());
            break;
        case StageRequest::None:
            break;
        }
    }

    // What the note over the picture says while a tool is picked.
    [[nodiscard]] std::string tool_hint() const {
        if (tool_ == StageTool::Delete) {
            return "Click a thing to delete it";
        }
        const SceneItem* item = document_.scene->item(scene_view_.selected());
        if (item == nullptr || !std::holds_alternative<Object>(item->data)) {
            return tool_ == StageTool::Place ? "Select an object to place copies of it"
                                             : "Select an object to drop copies of it";
        }
        if (tool_ == StageTool::Place) {
            return "Click to place " + item->name + " on the stage (" +
                   std::to_string(stage_count(document_.scene->performance)) + " there)";
        }
        const std::size_t held = dropped_count(document_.scene->performance);
        return "Click to drop a copy of " + item->name + " (" + std::to_string(held) + " of " +
               std::to_string(document_.scene->physics.max_dropped) + " dropped)";
    }

    // The scene's performance as actions for the package the preview plays:
    // made again when either changes, saying from which step it differs.
    void follow_performance() {
        const Performance& now = document_.scene->performance;
        const bool same_package = actions_ && actions_generation_ == package_generation_;
        if (same_package && now == actions_of_) {
            return;
        }
        actions_from_ = same_package ? first_difference(actions_of_, now).value_or(0U) : 0U;
        actions_ = performance_actions(now, package_targets_);
        actions_of_ = now;
        actions_generation_ = package_generation_;
    }

    const rhi::Texture* render_preview(const gui::Rect& view) {
        if (!preview_) {
            return nullptr;
        }
        rhi::Device& device = window_.device();
        // Finished frames report their GPU time; at most two are in flight.
        for (Flight& flight : flights_) {
            if (flight.work && flight.work.complete()) {
                const auto gpu = flight.work.gpu_execution();
                if (gpu.count() > 0) {
                    gpu_ms_ = gpu_ms_ * 0.9 + std::chrono::duration<double, std::milli>(gpu).count() * 0.1;
                }
                flight.work = rhi::SubmittedWork{};
            }
        }
        Flight* slot = nullptr;
        for (Flight& flight : flights_) {
            if (!flight.work) {
                slot = &flight;
                break;
            }
        }
        if (slot == nullptr) {
            return last_picture_;
        }
        const float scale = window_.ui_scale();
        PreviewFrame frame;
        frame.width = static_cast<std::uint32_t>(std::clamp(view.width * scale, 16.0F, 3840.0F));
        frame.height = static_cast<std::uint32_t>(std::clamp(view.height * scale, 16.0F, 2160.0F));
        frame.backdrop = document_.backdrop;
        frame.glow = document_.glow;
        frame.figure = figure_;
        frame.staging_floor = false;
        frame.orbit = orbit_;
        frame.package = has_package_ ? builder_.preview_path().string() : std::string{};
        frame.package_generation = package_generation_;
        frame.frame = static_cast<std::uint64_t>(frame_);
        frame.revision = package_generation_;
        frame.paused = !playing_;
        follow_performance();
        frame.actions = actions_;
        frame.actions_from = actions_from_;
        // Scenes step once a show frame (scene_step_rate at 60 frames a second).
        shown_tick_ = frame.frame;
        if (has_package_ && package_live_ != 0U) {
            for (const LiveParameter& live : live_parameters(document_, package_live_)) {
                for (std::uint32_t c = 0U; c < live.components; ++c) {
                    ms::scene::ParticleParameter parameter;
                    parameter.key = live.name + "." + std::to_string(c);
                    parameter.base = live.value[c];
                    frame.parameters.push_back(std::move(parameter));
                }
            }
        }
        auto acquired = device.acquire_commands();
        if (!acquired) {
            return last_picture_;
        }
        rhi::CommandList commands = std::move(acquired).value();
        const rhi::Texture* picture = preview_->render(commands, frame);
        auto submitted = device.submit(std::move(commands));
        if (!submitted) {
            return nullptr;
        }
        slot->work = std::move(submitted).value();
        last_picture_ = picture;
        return picture;
    }

    // Where the transport's scrub bar runs, so the tracks line up under it.
    [[nodiscard]] static gui::Rect scrub_track(const gui::Rect& bar) {
        const float time_w = 96.0F;
        return gui::Rect{bar.x + 150.0F, bar.center_y() - 10.0F, std::max(bar.width - 150.0F - time_w - 12.0F, 20.0F), 20.0F};
    }

    // One row a track: the item's name on the left, its moments on the right
    // under the scrub bar. A point is a diamond, a span a bar; dragging either
    // writes the authored value. Clicking the name selects the item.
    void draw_timeline(const gui::Rect& area, const std::vector<Track>& tracks, const gui::Rect& scrub) {
        const gui::Theme& theme = ctx_.theme();
        const double frames = loop_frames();
        const float seconds = static_cast<float>(frames / 60.0);
        const float offset = timeline_scroll_.begin(ctx_, area);
        float y = area.y + 4.0F - offset;
        const float label_w = std::max(scrub.x - area.x - 12.0F, 40.0F);
        const auto at_x = [&](float t) { return scrub.x + scrub.width * std::clamp(t / seconds, 0.0F, 1.0F); };
        const auto to_seconds = [&](float x) { return std::clamp((x - scrub.x) / scrub.width, 0.0F, 1.0F) * seconds; };
        for (const Track& track : tracks) {
            const gui::Rect row{area.x, y, area.width, track_height};
            const bool selected = std::find(scene_view_.selection().begin(), scene_view_.selection().end(), track.item) !=
                                  scene_view_.selection().end();
            const gui::Rect name{area.x, y, label_w, track_height};
            const gui::Interaction pick = ctx_.grab(part_id(track.item, "track.name"), name);
            ctx_.cursor_for(pick, gui::Cursor::Hand);
            if (selected || pick.hovered) {
                ctx_.rounded(row, selected ? gui::fade(theme.selection, 0.5F) : theme.row, theme.radius);
            }
            ctx_.label(gui::Rect{name.x + 8.0F, name.y, name.width - 8.0F, name.height}, track.name,
                       selected ? theme.text : theme.text_muted);
            ctx_.tooltip(name, "Select " + track.name);
            if (pick.clicked) {
                scene_view_.select(track.item, ctx_.ctrl_held());
            }
            // A faint line the moments sit on, under the scrub bar.
            ctx_.panel(gui::Rect{scrub.x, row.center_y(), scrub.width, 1.0F}, theme.divider);
            std::uint32_t k = 0U;
            for (const TrackMarker& marker : track.markers) {
                const gui::UiColor tone = marker.draggable ? theme.keyframe : theme.text_muted;
                const std::string when = format_number(marker.seconds, 2) + " s";
                if (marker.duration > 0.0F) {
                    const float x0 = at_x(marker.seconds);
                    const float x1 = at_x(marker.seconds + marker.duration);
                    const gui::Rect span{x0, row.center_y() - 5.0F, std::max(x1 - x0, 4.0F), 10.0F};
                    const gui::Interaction drag = marker.draggable ? ctx_.grab(part_id(track.item, "track.span", k), span)
                                                                   : gui::Interaction{};
                    const gui::Rect end{x1 - 5.0F, row.center_y() - 7.0F, 10.0F, 14.0F};
                    const gui::Interaction stretch = marker.draggable ? ctx_.grab(part_id(track.item, "track.end", k), end)
                                                                      : gui::Interaction{};
                    ctx_.cursor_for(drag, gui::Cursor::ResizeHorizontal);
                    ctx_.cursor_for(stretch, gui::Cursor::ResizeHorizontal);
                    ctx_.rounded(span, (drag.held || drag.hovered) ? theme.accent : gui::fade(tone, 0.6F), 4.0F);
                    if (marker.draggable) {
                        ctx_.rounded(end, (stretch.held || stretch.hovered) ? theme.accent : tone, 3.0F);
                    }
                    ctx_.tooltip(span, std::string{marker.label()} + " from " + when + " for " +
                                           format_number(marker.duration, 2) + " s" +
                                           (marker.draggable ? ". Drag it; drag its end for how long." : ""));
                    if (stretch.held) {
                        if (stretch.pressed) { drag_grip_ = 0.0F; }
                        static_cast<void>(set_marker_duration(document_, marker, to_seconds(ctx_.pointer_x()) - marker.seconds));
                    } else if (drag.held) {
                        if (drag.pressed) { drag_grip_ = to_seconds(ctx_.pointer_x()) - marker.seconds; }
                        static_cast<void>(set_marker_time(document_, marker, to_seconds(ctx_.pointer_x()) - drag_grip_));
                    }
                } else {
                    const float x = at_x(marker.seconds);
                    const gui::Rect hit{x - 8.0F, row.y + 2.0F, 16.0F, track_height - 4.0F};
                    const bool act = marker.kind == MarkerKind::Drop || marker.kind == MarkerKind::Delete;
                    const gui::Interaction drag =
                        marker.draggable ? ctx_.grab(part_id(track.item, act ? "track.act" : "track.point",
                                                             act ? marker.part : k),
                                                     hit)
                                         : gui::Interaction{};
                    ctx_.cursor_for(drag, gui::Cursor::ResizeHorizontal);
                    const bool picked = act && marker.part == selected_act_;
                    if (act && drag.pressed) {
                        selected_act_ = marker.part;
                    }
                    const bool lit = drag.held || drag.hovered || picked;
                    const gui::UiColor act_tone = marker.kind == MarkerKind::Delete ? theme.warning : theme.good;
                    editor::draw_diamond(ctx_, x, row.center_y(), lit ? 7.0F : (act ? 5.0F : 6.0F),
                                         lit ? theme.accent : (act ? act_tone : tone));
                    ctx_.tooltip(hit, std::string{marker.label()} + " at " + when +
                                          (act ? ". Drag to move it; Delete takes it away."
                                               : marker.draggable ? ". Drag it." : ""));
                    if (drag.held) {
                        static_cast<void>(set_marker_time(document_, marker, to_seconds(ctx_.pointer_x())));
                    }
                }
                ++k;
            }
            y += track_height;
        }
        // The playhead runs down through the tracks too.
        const float head_x = scrub.x + scrub.width * std::clamp(static_cast<float>(frame_ / frames), 0.0F, 1.0F);
        ctx_.rounded(gui::Rect{head_x - 0.5F, area.y, 1.0F, area.height}, gui::fade(theme.playhead, 0.5F), 0.5F);
        timeline_scroll_.end(ctx_, y + offset - area.y + 4.0F);
    }

    void draw_transport(const gui::Rect& bar) {
        const gui::Theme& theme = ctx_.theme();
        const float y = bar.y + (bar.height - 32.0F) * 0.5F;
        if (editor::tool_key(ctx_, gui::ui_id("studio.restart"), gui::Rect{bar.x, y, 36.0F, 32.0F}, gui::Icon::ToStart,
                             false, "Back to the start (Home)", theme.accent)) {
            frame_ = 0.0;
        }
        if (editor::tool_key(ctx_, gui::ui_id("studio.play"), gui::Rect{bar.x + 42.0F, y, 52.0F, 32.0F},
                             playing_ ? gui::Icon::Pause : gui::Icon::Play, playing_,
                             playing_ ? "Pause (Space)" : "Play (Space)", theme.good)) {
            toggle_playing();
        }
        if (editor::tool_key(ctx_, gui::ui_id("studio.loop"), gui::Rect{bar.x + 100.0F, y, 36.0F, 32.0F}, gui::Icon::Loop,
                             looping_, looping_ ? "Looping: plays round and round (L)" : "Plays once and stops (L)",
                             theme.accent)) {
            looping_ = !looping_;
        }

        // The loop as a bar to scrub, a tick a second.
        const double frames = loop_frames();
        const std::string time = format_number(static_cast<float>(frame_ / 60.0), 1) + " / " +
                                 format_number(static_cast<float>(frames / 60.0), 1) + " s";
        const float time_w = 96.0F;
        const gui::Rect track = scrub_track(bar);
        const gui::Interaction hit = ctx_.grab(gui::ui_id("studio.scrub"), track.inflated(0.0F, 6.0F));
        ctx_.cursor_for(hit, gui::Cursor::ResizeHorizontal);
        ctx_.rounded(gui::Rect{track.x, track.center_y() - 3.0F, track.width, 6.0F}, theme.track, 3.0F);
        const float t = std::clamp(static_cast<float>(frame_ / frames), 0.0F, 1.0F);
        ctx_.rounded(gui::Rect{track.x, track.center_y() - 3.0F, track.width * t, 6.0F}, gui::fade(theme.playhead, 0.35F), 3.0F);
        const int seconds = static_cast<int>(frames / 60.0);
        for (int s = 1; s < seconds; ++s) {
            const float tx = track.x + track.width * static_cast<float>(s * 60.0 / frames);
            ctx_.panel(gui::Rect{tx, track.center_y() + 5.0F, 1.0F, 5.0F}, theme.text_muted);
        }
        // What the cache holds: a band over the steps it covers, green while
        // it holds the scene as it is, amber once it is stale.
        if (bake_status_.any_baked && bake_status_.cached_frames > 0U) {
            const float covered =
                std::clamp(static_cast<float>(static_cast<double>(bake_status_.cached_frames) / frames), 0.0F, 1.0F);
            const gui::Rect band{track.x, track.y - 1.0F, track.width * covered, 3.0F};
            ctx_.rounded(band, bake_status_.current ? theme.good : theme.caution, 1.5F);
            ctx_.tooltip(gui::Rect{band.x, band.y - 3.0F, band.width, band.height + 6.0F},
                         bake_status_.current ? "Steps the cache holds."
                                              : "Steps the cache holds, now stale. Bake again.");
        }
        const float head_x = track.x + track.width * t;
        ctx_.rounded(gui::Rect{head_x - 1.5F, track.y - 2.0F, 3.0F, track.height + 4.0F}, theme.playhead, 1.5F);
        if (hit.held) {
            const float u = std::clamp((ctx_.pointer_x() - track.x) / track.width, 0.0F, 1.0F);
            frame_ = std::min(static_cast<double>(u) * frames, frames - 1.0);
            playing_ = false;
        }
        ctx_.label(gui::Rect{track.right() + 12.0F, bar.y, time_w, bar.height}, time, theme.text_muted);
    }

    struct Flight final {
        rhi::SubmittedWork work;
    };

    gui::UiWindow& window_;
    gui::FontAtlas& font_;
    gui::InputState& input_;
    gui::UiDrawList& list_;
    gui::UiContext& ctx_;

    PackageBuilder builder_;
    std::unique_ptr<Preview> preview_;
    std::string preview_error_;
    SceneView scene_view_;
    gui::ScrollRegion timeline_scroll_;
    float drag_grip_{}; // seconds between a dragged span's start and where it was taken
    float scene_tree_split_{0.19F};
    float scene_inspector_split_{0.66F};
    Document scene_guides_;
    std::uint64_t scene_guides_for_{};
    StageOverlay overlay_;
    // The viewport and timeline, as last drawn: where letter shortcuts act.
    gui::Rect stage_area_{};
    SessionState session_;
    bool show_guides_{true};
    bool show_handles_{true};
    bool show_paths_{false};
    std::string asset_issue_;
    // Dear ImGui, drawn over the rest while Studio is moved onto it a panel
    // at a time (imgui_layer.hpp), and what the Stage panel keeps.
    std::unique_ptr<ImguiLayer> imgui_;
    StagePanelState stage_panel_;
    bool settling_{false};
    std::string stage_status_;
    // The picture as the camera sees it, in the interface's units, for the
    // gizmo over the thing picked on the stage.
    gui::Rect stage_view_{};
    // Models dropped on the viewport from the desktop: each is a kind placed
    // by hand, and a first copy goes on the stage where it was let go once
    // the package that has the kind is built.
    struct PendingPlace final {
        Uid kind{};
        Vec3 at{};
        double since{};
    };
    std::vector<PendingPlace> pending_places_;
    std::map<std::string, std::vector<Vec3>, std::less<>> outlines_;
    std::uint32_t settled_frames_{0U};
    // What a click in the picture does, and the act last made or picked on
    // the timeline (0 for none).
    StageTool tool_{StageTool::Select};
    std::uint32_t selected_act_{0U};
    // Where the package the preview plays takes acts, the performance as
    // actions for it, what they were made of, for which package and from
    // which step they differ from the last; and the step the preview last
    // drew, which an act is stamped from.
    ActTargets package_targets_;
    std::shared_ptr<const ActionList> actions_;
    Performance actions_of_;
    std::uint64_t actions_generation_{0U};
    std::uint64_t actions_from_{0U};
    std::uint64_t shown_tick_{0U};
    editor::ClickTracker clicks_;

    Document document_;
    // The page as last seen to change, and the copies it is compared with:
    // as saved, as sent to the builder, as compiled and as the newest step
    // of the history. Each frame looks at the page once (`changes_`); the
    // rest ask the number that leaves, so a page that is not being edited
    // costs one comparison a frame, not one for every question.
    PageRevision changes_;
    PageMark saved_;
    PageMark submitted_;
    // The system the last package sent reads live, and the one the package
    // written last reads live.
    Uid submitted_live_{0U};
    Uid package_live_{0U};
    // The page as the CPU run was last asked for, and when.
    PageMark analysed_;
    double last_analysis_{0.0};
    // preview_current's answer, kept for the change it was worked out for.
    bool preview_shows_page_{true};
    bool live_only_{false};
    std::uint64_t live_checked_at_{0U};
    std::uint64_t live_checked_serial_{0U};
    std::uint64_t submit_serial_{0U};
    PageMark compiled_for_;
    PageMark history_head_;
    Compiled compiled_;
    // Compiles each edit off the interface thread; the revision last handed
    // to it, so a page is handed over once.
    CompileWorker compiler_;
    std::uint64_t compile_asked_{0U};
    // What `compiled_` came to, kept with it.
    float natural_loop_{2.0F};
    std::uint64_t compiled_serial_{0U};
    std::vector<Document> history_;
    std::size_t history_index_{0U};
    std::filesystem::path path_;
    std::uint64_t revision_{0U};
    // The first revision of the scene now open, whose CPU runs it shows.
    std::uint64_t analysis_from_{0U};
    double last_submit_{0.0};
    std::string build_error_;
    double kernel_ms_{0.0};
    std::optional<Analysis> analysis_;
    // The newest bake, what it holds of the page as it is, and the page that
    // was worked out against.
    std::shared_ptr<const BakeRecord> bake_;
    BakeStatus bake_status_;
    PageMark bake_status_for_;
    std::uint64_t bake_serial_{0U};
    bool bake_status_dirty_{true};
    std::string bake_label_;
    std::string bake_tip_;
    // Which page a bake was asked for, against the page open now.
    std::uint64_t page_generation_{0U};
    std::uint64_t bake_asked_for_{0U};
    // The bake under way was Studio's own, taken once the page settled.
    bool bake_quiet_{false};
    // Why the last bake Studio took by itself failed, so it is said once.
    std::string auto_bake_failure_;
    // The page revision last baked by itself, and when the page last changed.
    std::uint64_t auto_baked_for_{0U};
    std::uint64_t change_seen_{0U};
    double change_time_{0.0};

    bool has_package_{false};
    std::uint64_t package_generation_{0U};
    double frame_{0.0};
    bool playing_{true};
    bool looping_{true};
    double last_time_{0.0};
    Orbit orbit_{};
    bool want_frame_{true};
    float preview_aspect_{16.0F / 9.0F};
    bool panning_{false};
    bool dollying_{false};
    bool figure_{true};
    std::array<Flight, 2> flights_{};
    const rhi::Texture* last_picture_{nullptr};
    double gpu_ms_{0.0};
    Toast toast_;
    bool night_{false};
    double lose_device_at_{-1.0};
};

} // namespace

int run_studio(const StudioOptions& options) {
    // Where Studio was left, unless asked to start afresh.
    SessionState session;
    if (const std::filesystem::path file = session_path(); !file.empty() && !options.fresh) {
        std::string error;
        if (const std::optional<std::vector<std::uint8_t>> bytes = read_file(file, error)) {
            session = load_session(std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()});
        }
    }
    gui::WindowConfig config{};
    config.title = "ajfx Studio";
    config.width = session.width != 0U ? session.width : 1600U;
    config.height = session.height != 0U ? session.height : 940U;
    config.logical_size = true;
    config.debug = options.gpu_debug;
    auto created = gui::UiWindow::create(config);
    if (!created) {
        std::cerr << "ajfx Studio could not open its window: no display or no usable GPU\n";
        return 1;
    }
    gui::UiWindow window = std::move(created).value();
    window.set_zoom(session.zoom);
    gui::FontAtlas font = load_font(window.ui_scale());
    if (!window.set_atlas(font)) {
        std::cerr << "ajfx Studio could not upload its font\n";
        return 1;
    }
    gui::InputState input;
    gui::UiDrawList list;
    gui::UiContext ctx{input, list, font};
    {
        Studio studio{window, font, input, list, ctx};
        if (!studio.start(options, std::move(session))) {
            return 1;
        }
        while (studio.frame()) {
        }
        window.device().wait_idle();
    }
    return 0;
}

} // namespace ajfx::studio
