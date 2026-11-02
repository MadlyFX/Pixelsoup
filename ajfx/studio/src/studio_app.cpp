#include "studio_app.hpp"

#include "bake.hpp"
#include "budget.hpp"
#include "budget_meter.hpp"
#include "coach.hpp"
#include "compile.hpp"
#include "compile_worker.hpp"
#include "document_io.hpp"
#include "package_builder.hpp"
#include "page_revision.hpp"
#include "page_view.hpp"
#include "palette.hpp"
#include "preview.hpp"
#include "recipes.hpp"
#include "sandbox.hpp"
#include "sandbox_tools.hpp"
#include "session.hpp"
#include "scene_view.hpp"
#include "stage_camera.hpp"
#include "stage_overlay.hpp"
#include "widgets.hpp"

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/shapes.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/gui/window.hpp>

#include <tinyfiledialogs.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
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
// A page with a solid world bakes it by itself once nothing has changed for
// this long and the preview is up to date.
constexpr double auto_bake_delay = 0.8;

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
        split_ = session_.split;
        palette_.set_open(session_.palette);
        palette_.set_folded(session_.palette_folded);
        show_guides_ = session_.guides;
        show_handles_ = session_.handles;
        show_paths_ = session_.paths;
        figure_ = session_.figure;
        expert_ = session_.expert;
        night_ = session_.night;
        looping_ = session_.looping;

        std::optional<Document> recipe = options.recipe.empty() ? std::optional<Document>{new_scene()} : make_recipe(options.recipe);
        if (!recipe) { recipe = new_scene(); }
        const bool resumed = options.open.empty() && options.restore_page && resume_page();
        if (!resumed && (options.open.empty() || !open_path(options.open))) {
            replace_document(std::move(*recipe), {});
        }
        expert_ = expert_ || options.expert;
        for (std::size_t l = 0U; l < document_.layers.size(); ++l) {
            for (const Block& block : document_.layers[l].blocks) {
                if (block.kind == options.open_blocks || (options.open_blocks == "all" && l == 0U)) {
                    page_.open_block(block.uid);
                }
            }
        }
        open_picker_ = options.picker;
        maximize_at_start_ = options.maximize;
        report_budget_ = options.maximize;
        looping_ = looping_ && !options.once;
        show_guides_ = options.guides.value_or(show_guides_);
        show_handles_ = options.guides.value_or(show_handles_);
        show_paths_ = options.paths.value_or(show_paths_);
        if (options.lose_device >= 0.0F) {
            lose_device_at_ = window_.time_seconds() + static_cast<double>(options.lose_device);
        }
        for (const std::string& attempt : options.tries) {
            const std::size_t equals = attempt.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            ParameterValue value{};
            std::size_t component = 0U;
            for (std::size_t at = equals + 1U; at < attempt.size() && component < 4U; ++component) {
                std::size_t end = attempt.find(',', at);
                if (end == std::string::npos) {
                    end = attempt.size();
                }
                value[component] = std::strtof(attempt.substr(at, end - at).c_str(), nullptr);
                at = end + 1U;
            }
            page_.try_value(attempt.substr(0U, equals), value);
        }
        if (options.at >= 0.0F) {
            frame_ = static_cast<double>(options.at) * 60.0;
            playing_ = false;
        }
        scene_tree_split_ = session_.scene_tree_split;
        scene_inspector_split_ = session_.scene_inspector_split;
        scene_view_.restore(session_.scene_selection, session_.scene_folded);
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
        const double now = window_.time_seconds();
        const double dt = std::clamp(now - last_time_, 0.0, 0.1);
        last_time_ = now;
        ctx_.set_theme(studio_theme(night_));
        ctx_.new_frame(now);

        take_results(now);
        advance_playback(dt);
        lose_device_if_asked(now);

        const float width = static_cast<float>(window_.width());
        const float height = static_cast<float>(window_.height());
        const gui::Rect window{0.0F, 0.0F, width, height};
        const gui::Theme& theme = ctx_.theme();
        ctx_.panel(window, theme.background);

        // An open popover owns the pointer where its card is. The gallery is
        // drawn after the page's own picker, so it covers that too.
        const bool under_gallery = gallery_.covers(ctx_.pointer_x(), ctx_.pointer_y());
        PopoverShield shield{ctx_, under_gallery || (document_.scene ? scene_view_.covers(ctx_.pointer_x(), ctx_.pointer_y()) : page_.popover_covers(ctx_.pointer_x(), ctx_.pointer_y()))};

        draw_top_bar(gui::Rect{0.0F, 0.0F, width, top_bar_height});
        const gui::Rect body{10.0F, top_bar_height, std::max(width - 20.0F, 10.0F), std::max(height - top_bar_height - 10.0F, 10.0F)};
        // The palette down the left, then the page and the preview.
        if (document_.scene) {
            // The viewport owns the center; tree and inspector are workspace
            // panels with independent resize handles.
            const gui::Split left = ctx_.split(gui::ui_id("scene.tree.split"), body, scene_tree_split_, gui::Axis::Row, 8.0F);
            const gui::Split right = ctx_.split(gui::ui_id("scene.inspector.split"), left.second, scene_inspector_split_, gui::Axis::Row, 8.0F);
            scene_view_.draw_tree(ctx_, left.first, document_);
            scene_view_.draw_inspector(ctx_, right.second, document_, compiled_.error.empty() ? std::string_view{build_error_} : std::string_view{compiled_.error});
            draw_preview(right.first, now);
            shield.release();
            scene_view_.draw_overlays(ctx_, window, document_);
        } else {
        const float palette_w = palette_.is_open() ? Palette::open_width : Palette::rail_width;
        const gui::Rect palette_rect{body.x, body.y, palette_w, body.height};
        const gui::Rect rest{body.x + palette_w + 10.0F, body.y, std::max(body.width - palette_w - 10.0F, 10.0F),
                             body.height};
        const gui::Split split = ctx_.split(gui::ui_id("studio.split"), rest, split_, gui::Axis::Row, 10.0F);

        const std::vector<Advice>& advice = current_advice();
        PageInputs inputs{&compiled_, &advice, analysis_ ? &*analysis_ : nullptr, expert_, natural_loop_};
        inputs.bake = &bake_status_;
        inputs.budget = &budget_view_;
        palette_.draw(ctx_, palette_rect, document_, page_.target_layer(document_));
        page_.set_incoming(palette_.held());
        page_.draw(ctx_, split.first, document_, inputs);
        if (std::optional<Incoming> clicked = palette_.take_clicked()) {
            page_.add(document_, *clicked);
        }
        draw_preview(split.second, now);
        shield.release();
        {
            const PopoverShield over_page{ctx_, under_gallery};
            page_.draw_overlays(ctx_, window, document_, inputs);
            palette_.end_frame(ctx_);
            page_.set_incoming(nullptr);
        }
        draw_gallery(window);
        }
        draw_toast(window, now);
        ctx_.draw_overlays(window);

        if (open_picker_ && frames_ > 2U) {
            page_.open_block_picker(document_);
            open_picker_ = false;
        }
        ++frames_;
        // Whatever drew edited the page in place; this is the one look at it
        // each frame, and what follows asks the number it leaves.
        changes_.observe(document_);
        if (document_.scene && scene_motion_for_ != changes_.value()) {
            if (!same_scene_motion(scene_motion_, document_)) { frame_ = 0.0; playing_ = false; }
            scene_motion_ = document_;
            scene_motion_for_ = changes_.value();
        }
        if (std::optional<BudgetRequest> asked = page_.take_budget_request()) {
            budget_request(*asked);
        }
        if (maximize_at_start_ && has_package_ && analysis_ && preview_ && !preview_->status().waiting) {
            maximize_at_start_ = false;
            budget_request(BudgetRequest{});
        }
        budget_step();
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
                // Whatever was saved or not, the page comes back next time.
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

    // The coach's notes, worked out again only when the page, what it
    // compiled to or its bake status has changed.
    const std::vector<Advice>& current_advice() {
        const std::array<std::uint64_t, 3> key{changes_.value(), compiled_serial_, bake_serial_};
        if (key != advice_key_) {
            advice_ = advise(document_, compiled_, &bake_status_);
            advice_key_ = key;
        }
        return advice_;
    }

    void replace_document(Document document, std::filesystem::path path) {
        document_ = std::move(document);
        scene_motion_ = document_;
        path_ = std::move(path);
        const std::uint64_t at = changes_.touch(document_);
        saved_.set(document_, at);
        history_.assign(1U, document_);
        history_index_ = 0U;
        history_head_.set(document_, at);
        page_.forget();
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
        playing_ = !document_.scene;
        if (document_.scene) { looping_ = false; }
        submit(window_.time_seconds());
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
        // How the page now open was laid out, kept for when it opens again.
        if (!path_.empty()) {
            remember_view(session_, path_.generic_string(), page_.view_state());
        }
        replace_document(std::move(*loaded.document), path);
        if (const std::optional<PageViewState> view = remembered_view(session_, path.generic_string())) {
            page_.restore(*view);
        }
        toast("Opened " + path.filename().string(), true);
        return true;
    }

    // The page Studio was left on, with its unsaved changes, its layout, the
    // camera and the playhead. False when there is none to take up.
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
                // On top of the file, so the page shows it has changes to save.
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
        page_.restore(session_.view);
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

    // Writes where Studio is, for next time. `discarded`: the page had
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
        session.view = page_.view_state();
        remember_view(session, session.page, session.view);
        // Window creation applies display scale; zoom is restored separately.
        session.width = static_cast<std::uint32_t>(std::lround(static_cast<float>(window_.width()) * window_.zoom()));
        session.height = static_cast<std::uint32_t>(std::lround(static_cast<float>(window_.height()) * window_.zoom()));
        session.zoom = window_.zoom();
        session.split = split_;
        session.palette = palette_.is_open();
        session.palette_folded = palette_.folded();
        session.guides = show_guides_;
        session.handles = show_handles_;
        session.paths = show_paths_;
        session.figure = figure_;
        session.expert = expert_;
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
    // window of the system's own, which still works, offers to save the page,
    // and lets Studio close.
    void device_lost(std::string_view reason) {
        std::string message = "ajfx Studio can no longer draw: " + std::string{reason} + ".\n\n";
        if (!unsaved()) {
            message += "The page has no unsaved changes. Studio will close; open it again to carry on.";
            tinyfd_messageBox("ajfx Studio", message.c_str(), "ok", "error", 1);
            return;
        }
        message += "Save the page before Studio closes?";
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

    // Whether the page has changes the file does not. Looked at afresh,
    // since the callers are about to keep or discard the page.
    bool unsaved() {
        return saved_.differs(document_, changes_.observe(document_));
    }

    bool confirm_discard(const char* action) {
        if (!unsaved()) {
            return true;
        }
        const std::string message = "This effect has changes that are not saved. Discard them and " + std::string{action} + "?";
        return tinyfd_messageBox("ajfx Studio", message.c_str(), "yesno", "question", 0) == 1;
    }

    void open_dialog() {
        if (!confirm_discard("open another page")) {
            return;
        }
        const char* filters[] = {"*.ajfxstudio"};
        if (const char* chosen = tinyfd_openFileDialog("Open an ajfx Studio page", "", 1, filters, "ajfx Studio pages", 0)) {
            static_cast<void>(open_path(chosen));
        }
    }

    bool save(bool choose) {
        std::filesystem::path path = path_;
        if (choose || path.empty()) {
            const char* filters[] = {"*.ajfxstudio"};
            const std::string suggestion = (document_.name.empty() ? std::string{"effect"} : document_.name) + ".ajfxstudio";
            const char* chosen = tinyfd_saveFileDialog("Save the page", suggestion.c_str(), 1, filters, "ajfx Studio pages");
            if (chosen == nullptr) {
                return false;
            }
            path = chosen;
            if (path.extension() != document_extension) {
                path += document_extension;
            }
        }
        const std::string text = save_document(document_);
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
        // The cache goes with it only while it holds every baked layer as
        // it is: the export checks the rest as a host will.
        builder_.submit_export(document_, path, path_.parent_path(), bake_status_.current ? bake_ : nullptr);
        toast("Exporting " + path.filename().string() + "...", true);
    }

    // Bakes the page's baked layers, or stops the bake under way.
    void bake_or_cancel() {
        if (builder_.bake_progress() >= 0.0) {
            builder_.cancel_bake();
            return;
        }
        if (!bake_status_.any_baked) {
            toast("Nothing to bake: mark a layer Baked on its heading first", false);
            return;
        }
        builder_.submit_bake(document_, path_.parent_path());
        bake_asked_for_ = page_generation_;
        bake_quiet_ = false;
    }

    // What baking the page now would come to, in a sentence.
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

    void submit(double now) {
        ++revision_;
        builder_.submit(document_, revision_, path_.parent_path());
        submitted_.set(document_, changes_.value());
        last_submit_ = now;
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
                ++package_generation_;
                settled_frames_ = 0U;
                has_package_ = true;
                kernel_ms_ = built->kernel_ms;
            } else if (!built->compiled.effect && built->compiled.error.empty()) {
                has_package_ = false;
            }
        }
        if (std::optional<Analysis> analysis = builder_.take_analysis()) {
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
            // A bake of a page since replaced is not this page's.
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
        // What the baked layers' cache holds, against the page as it is. It
        // compiles the page as the export does, so only when either changes,
        // and only once the preview's compile has caught up with the page.
        if (!compile_behind && (bake_status_dirty_ || bake_status_for_.differs(document_, at))) {
            // The preview's compile is the export's unless a layer plays
            // alone, so it is compiled again only then.
            const bool soloed = std::any_of(document_.layers.begin(), document_.layers.end(),
                                            [](const Layer& layer) { return layer.solo && layer.enabled; });
            bake_status_ = bake_status(document_, soloed ? compile(document_, Purpose::Export) : compiled_,
                                       bake_.get(), analysis_ ? &analysis_->peak_alive : nullptr);
            // Studio bakes the solid world by itself once the page settles.
            bake_status_.automatic = true;
            bake_status_for_.set(document_, at);
            ++bake_serial_;
            bake_status_dirty_ = false;
        }
        bake_when_settled(now, at, compile_behind);
        if (preview_ && !preview_->status().waiting && has_package_) {
            ++settled_frames_;
        }
        if (submitted_.differs(document_, at)) {
            const bool live = (document_.scene ? scene_view_.gesture_live() : page_.gesture_live()) || ctx_.pointer_down() || overlay_.dragging();
            // While a value is dragged, the next package waits for the preview
            // to have taken in the last; the gesture's end always sends one.
            const bool settled = !builder_.busy() && (settled_frames_ >= settle_frames || !preview_ ||
                                                      now - last_submit_ >= settle_timeout);
            if (!live || (now - last_submit_ >= submit_interval && settled)) {
                submit(now);
            }
        }
    }

    // Bakes a page with a solid world by itself once it has settled: nothing
    // changed for a moment, nothing held, the preview built and the cache not
    // current. An edit stops a bake Studio took by itself, so a drag never
    // waits on one, and it is taken again once the page settles again. Once
    // a page, so a bake that fails is not tried again until the page changes.
    void bake_when_settled(double now, std::uint64_t at, bool compile_behind) {
        if (at != change_seen_) {
            change_seen_ = at;
            change_time_ = now;
            if (bake_quiet_ && builder_.bake_progress() >= 0.0) {
                builder_.cancel_bake();
            }
        }
        const bool held = (document_.scene ? scene_view_.gesture_live() : page_.gesture_live()) || ctx_.pointer_down() || overlay_.dragging();
        const bool settled = !compile_behind && !bake_status_dirty_ && !bake_status_for_.differs(document_, at) &&
                             now - change_time_ >= auto_bake_delay && !held && !builder_.busy() &&
                             !submitted_.differs(document_, at) && builder_.bake_progress() < 0.0;
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
        const bool gesture = (document_.scene ? scene_view_.gesture_live() : page_.gesture_live()) || ctx_.pointer_down() || ctx_.focus() != gui::UiId{} ||
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

    // The page as the history kept it at `index`.
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
        const std::string_view typed = ctx_.shortcut_text();
        if (typed.find(' ') != std::string_view::npos) {
            toggle_playing();
        }
        if (typed.find('l') != std::string_view::npos || typed.find('L') != std::string_view::npos) {
            looping_ = !looping_;
        }
        if (typed.find('/') != std::string_view::npos) {
            if (document_.scene) { scene_view_.open_add(20.0F, top_bar_height + 40.0F); }
            else { page_.open_block_picker(document_); }
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
            if (frame_ >= frames && (!document_.scene || looping_)) {
                if (looping_) {
                    frame_ = std::fmod(frame_, frames);
                } else {
                    frame_ = frames - 1.0;
                    playing_ = false;
                }
            }
        }
        // Short of the loop's length rather than its last whole frame: on a
        // display faster than 60 Hz a refresh advances less than a frame, and
        // the playhead still has to reach the end to wrap.
        frame_ = document_.scene && !looping_ ? std::max(frame_, 0.0) : std::clamp(frame_, 0.0, std::nextafter(frames, 0.0));
    }

    void toggle_playing() {
        // Play at the end of a run that does not loop starts it again.
        if (!document_.scene && !playing_ && !looping_ && frame_ >= loop_frames() - 1.0) {
            frame_ = 0.0;
        }
        playing_ = !playing_;
    }

    // ------------------------------------------------------- top bar

    void draw_top_bar(const gui::Rect& bar) {
        const gui::Theme& theme = ctx_.theme();
        const bool compact = bar.width < 1100.0F;
        float x = bar.x + 18.0F;
        label_sized(ctx_, gui::Rect{x, bar.y, 60.0F, bar.height}, "ajfx", theme.text, 1.2F);
        x += text_width_sized(ctx_, "ajfx", 1.2F) + 6.0F;
        if (!compact) {
            ctx_.label(gui::Rect{x, bar.y + 2.0F, 60.0F, bar.height}, "Studio", theme.text_muted);
            x += 70.0F;
        }

        const float y = bar.y + 11.0F;
        const float file_button_w = compact ? 64.0F : 84.0F;
        if (editor::pill_button(ctx_, gui::ui_id("studio.new"), gui::Rect{x, y, file_button_w, 30.0F}, gui::Icon::Plus, "New",
                                "Start a new scene with a Ground object")) {
            if (confirm_discard("start a new scene")) { replace_document(new_scene(), {}); }
        }
        x += file_button_w + 6.0F;
        if (editor::pill_button(ctx_, gui::ui_id("studio.open"), gui::Rect{x, y, file_button_w, 30.0F}, gui::Icon::Import,
                                "Open", "Open an ajfx Studio page")) {
            open_dialog();
        }
        x += file_button_w + 6.0F;
        if (editor::pill_button(ctx_, gui::ui_id("studio.save"), gui::Rect{x, y, file_button_w, 30.0F}, std::nullopt, "Save",
                                "Save the page (Ctrl+S)")) {
            static_cast<void>(save(false));
        }
        x += file_button_w + 6.0F;
        const float export_w = compact ? 118.0F : 150.0F;
        if (editor::pill_button(ctx_, gui::ui_id("studio.export"), gui::Rect{x, y, export_w, 30.0F}, gui::Icon::Publish,
                                "Export .ajfx", "Export a package the media server plays, kernels included",
                                editor::ButtonTone::Primary)) {
            export_dialog();
        }
        x += export_w + (compact ? 6.0F : 16.0F);
        {
            const double progress = builder_.bake_progress();
            const bool baking = progress >= 0.0;
            // Kept on the Studio, so they outlive the frame whatever the
            // button keeps of them.
            std::string& label = bake_label_;
            std::string& tip = bake_tip_;
            label = "Bake";
            tip.clear();
            if (baking) {
                label = "Baking " + std::to_string(static_cast<int>(std::lround(progress * 100.0))) + "%";
                tip = "Working out the baked layers on the CPU. Click to stop.";
            } else if (!bake_status_.any_baked) {
                tip = "Mark a layer Baked on its heading first.";
            } else if (bake_status_.current) {
                tip = "Cache is up to date. Baking again: " + bake_cost_text() + ".";
            } else {
                tip = "Cache the baked layers: " + bake_cost_text() + ".";
            }
            if (editor::pill_button(ctx_, gui::ui_id("studio.bake"), gui::Rect{x, y, 110.0F, 30.0F}, std::nullopt,
                                    label.c_str(), tip.c_str())) {
                bake_or_cancel();
            }
            if (bake_status_.any_baked && !baking) {
                // A dot on the button: green when the cache holds the page,
                // amber when it needs taking (again).
                ctx_.rounded(gui::Rect{x + 96.0F, y + 12.0F, 6.0F, 6.0F},
                             bake_status_.current ? theme.good : theme.caution, 3.0F);
            }
            x += 120.0F;
        }

        // The page's file, and whether it has unsaved changes.
        std::string file = path_.empty() ? std::string{"Not saved yet"} : path_.filename().string();
        const bool dirty = saved_.differs(document_, changes_.value());
        const gui::Rect file_rect{x, bar.y, std::max(bar.right() - x - (compact ? 210.0F : 330.0F), 0.0F), bar.height};
        ctx_.label(file_rect, file, theme.text_muted);
        if (dirty) {
            const float fw = ctx_.font_width(file);
            if (fw + 16.0F < file_rect.width) {
                ctx_.rounded(gui::Rect{file_rect.x + fw + 8.0F, bar.center_y() - 3.0F, 6.0F, 6.0F}, theme.caution, 3.0F);
                ctx_.tooltip(gui::Rect{file_rect.x, bar.y, fw + 16.0F, bar.height}, "Changed since it was saved");
            }
        }

        // Right: what the build is doing, history, and the expert view.
        float rx = bar.right() - 16.0F;
        rx -= 30.0F;
        if (document_.scene) {
            if (editor::icon_button(ctx_, gui::ui_id("studio.night"), gui::Rect{rx, y + 1.0F, 28.0F, 28.0F},
                                    gui::Icon::Night, night_ ? "Turn off night dim" : "Dim the workspace for night",
                                    night_)) {
                night_ = !night_;
            }
        } else if (editor::icon_button(ctx_, gui::ui_id("studio.expert"), gui::Rect{rx, y + 1.0F, 28.0F, 28.0F},
                                gui::Icon::Details,
                                expert_ ? "Hide what each block runs as" : "Show what each block runs as (under the hood)",
                                expert_)) {
            expert_ = !expert_;
        }
        rx -= 32.0F;
        if (editor::icon_button(ctx_, gui::ui_id("studio.redo"), gui::Rect{rx, y + 1.0F, 28.0F, 28.0F}, gui::Icon::Restart,
                                "Redo (Ctrl+Y)", false, history_index_ + 1U < history_.size())) {
            redo();
        }
        rx -= 32.0F;
        if (editor::icon_button(ctx_, gui::ui_id("studio.undo"), gui::Rect{rx, y + 1.0F, 28.0F, 28.0F}, gui::Icon::Reset,
                                "Undo (Ctrl+Z)", false, history_index_ > 0U)) {
            undo();
        }

        // The state of the effect, as a dot and a word.
        std::string state = "Up to date";
        gui::UiColor tone = theme.good;
        std::string tip = "The preview shows this page.";
        if (!preview_error_.empty()) {
            state = "No preview";
            tone = theme.warning;
            tip = preview_error_;
        } else if (!build_error_.empty()) {
            state = "Cannot build";
            tone = theme.warning;
            tip = build_error_;
        } else if (builder_.busy() || submitted_.differs(document_, changes_.value())) {
            state = "Updating";
            tone = theme.caution;
            tip = "Building the preview from your latest change.";
        } else if (!compiled_.error.empty()) {
            state = "A layer cannot play";
            tone = theme.warning;
            tip = compiled_.error;
        } else if (!asset_issue_.empty()) {
            state = "A file cannot be used";
            tone = theme.caution;
            tip = asset_issue_ + ". Export waits for it.";
        } else if (kernel_ms_ > 0.0) {
            tip += " Its kernels compiled in " + format_number(static_cast<float>(kernel_ms_), 0) + " ms.";
        }
        const std::string_view compact_state = state == "Up to date" ? "Ready" : state == "Updating" ? "Wait" : "Issue";
        const float state_w = compact ? ctx_.font_width(compact_state) + 16.0F : ctx_.font_width(state) + 28.0F;
        const gui::Rect chip{rx - state_w - 12.0F, y + 2.0F, state_w, 26.0F};
        ctx_.rounded(chip, theme.track, chip.height * 0.5F);
        if (!compact) { ctx_.rounded(gui::Rect{chip.x + 10.0F, chip.center_y() - 3.5F, 7.0F, 7.0F}, tone, 3.5F); }
        if (!compact) {
            ctx_.label(gui::Rect{chip.x + 22.0F, chip.y, chip.width - 22.0F, chip.height}, state, theme.text);
        } else {
            // A state is never color alone, even when the bar is compact.
            ctx_.label_centered(chip, compact_state, tone);
        }
        ctx_.tooltip(chip, state + ": " + tip);
    }

    // ------------------------------------------------------- recipes

    void open_gallery() {
        std::vector<ListEntry> entries;
        for (const Recipe& recipe : recipes()) {
            entries.push_back(ListEntry{"Recipes", std::string{recipe.title}, std::string{recipe.description}, true, {}});
        }
        const float width = static_cast<float>(window_.width());
        gallery_.open(width * 0.5F - 190.0F, top_bar_height + 30.0F, "Start a new effect", std::move(entries), false);
    }

    void draw_gallery(const gui::Rect& window) {
        if (std::optional<std::size_t> picked = gallery_.draw(ctx_, window)) {
            if (*picked < recipes().size() && confirm_discard("start a new effect")) {
                if (std::optional<Document> document = make_recipe(recipes()[*picked].id)) {
                    replace_document(std::move(*document), {});
                }
            }
        }
    }

    void draw_toast(const gui::Rect& window, double now) {
        if (toast_.text.empty() || now > toast_.until) {
            return;
        }
        const gui::Theme& theme = ctx_.theme();
        const float w = std::min(ctx_.font_width(toast_.text) + 44.0F, window.width - 40.0F);
        const gui::Rect card{window.center_x() - w * 0.5F, window.bottom() - 70.0F, w, 36.0F};
        ctx_.shadow(card, theme.radius_large);
        ctx_.rounded(card, theme.widget, card.height * 0.5F);
        ctx_.rounded(gui::Rect{card.x + 14.0F, card.center_y() - 3.5F, 7.0F, 7.0F}, toast_.ok ? theme.good : theme.warning,
                     3.5F);
        ctx_.label(gui::Rect{card.x + 28.0F, card.y, card.width - 36.0F, card.height}, toast_.text, theme.text);
    }

    // ------------------------------------------------------- the budget

    // "Maximize budget", "Scale down" or "Stop" from the page's budget row.
    // The page's limit is worked out off this thread, then the stage alone
    // is measured, then the page at each factor the search asks for, in the
    // meter's own preview; the page itself changes once, at the end.
    void budget_request(const BudgetRequest& asked) {
        if (asked.stop) {
            cancel_budget("Stopped");
            return;
        }
        if (budget_phase_ != BudgetPhase::Idle) {
            return;
        }
        if (!preview_) {
            budget_view_ = BudgetView{false, "Needs the preview", false};
            return;
        }
        if (!meter_) {
            std::filesystem::path package = builder_.preview_path();
            package.replace_filename("budget-" + package.filename().string());
            std::string error;
            meter_ = BudgetMeter::create(window_.device(), std::move(package), error);
            if (!meter_) {
                budget_view_ = BudgetView{false, error, false};
                return;
            }
        }
        budget_for_ = changes_.value();
        budget_options_ = asked.options;
        budget_down_ = asked.allow_down;
        budget_issue_.clear();
        // From a little before the busiest step, so the window it measures
        // is the effect at its heaviest, inside the loop.
        const double frames = loop_frames();
        const double peak = analysis_ && analysis_->traced && analysis_->peak_step != 0U
                                ? static_cast<double>(analysis_->peak_step)
                                : frames * 0.5;
        budget_from_ = static_cast<std::uint64_t>(std::clamp(std::min(peak - 15.0, frames - 40.0), 0.0, frames));
        budget_limit_ = std::async(std::launch::async, [page = document_, options = asked.options] {
            return density_limit(page, options);
        });
        budget_phase_ = BudgetPhase::Limiting;
        budget_view_ = BudgetView{true, "Getting ready", false};
    }

    void cancel_budget(std::string words) {
        if (budget_phase_ == BudgetPhase::Idle) {
            return;
        }
        if (meter_) {
            meter_->cancel();
        }
        budget_run_.reset();
        budget_phase_ = BudgetPhase::Idle;
        budget_view_ = BudgetView{false, std::move(words), false};
    }

    void budget_step() {
        if (budget_phase_ == BudgetPhase::Idle) {
            return;
        }
        // Measured against the page as it was: an edit makes it moot.
        if (changes_.value() != budget_for_) {
            cancel_budget("Stopped: the page changed");
            return;
        }
        if (budget_phase_ == BudgetPhase::Limiting) {
            if (budget_limit_.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
                return;
            }
            budget_run_.emplace(document_, budget_options_, budget_limit_.get(), budget_down_);
            if (budget_run_->done()) {
                finish_budget();
                return;
            }
            meter_->measure(nullptr, budget_from_, path_.parent_path());
            budget_phase_ = BudgetPhase::Stage;
            budget_view_.status = "Measuring";
            return;
        }
        if (budget_look_.width == 0U) {
            return;
        }
        meter_->frame(window_.device(), budget_look_);
        const std::optional<BudgetMeasure> measured = meter_->take();
        if (!measured) {
            return;
        }
        if (report_budget_) {
            std::cout << (budget_phase_ == BudgetPhase::Stage
                              ? std::string{"stage"}
                              : "factor " + std::to_string(budget_run_->next().value_or(1.0)))
                      << ": " << measured->ms << " ms a frame" << (measured->issue.empty() ? "" : ", ")
                      << measured->issue << std::endl;
        }
        if (budget_phase_ == BudgetPhase::Stage) {
            if (!std::isfinite(measured->ms)) {
                cancel_budget(measured->issue);
                return;
            }
            budget_stage_ms_ = measured->ms;
            budget_phase_ = BudgetPhase::Probing;
        } else {
            // What the page adds to the stage alone.
            if (!measured->issue.empty()) {
                budget_issue_ = measured->issue;
            }
            budget_run_->report(std::isfinite(measured->ms) ? std::max(measured->ms - budget_stage_ms_, 0.0)
                                                             : std::numeric_limits<double>::infinity());
            if (budget_run_->done()) {
                finish_budget();
                return;
            }
        }
        const std::optional<double> factor = budget_run_->next();
        const Document probe = budget_run_->page_at(factor.value_or(1.0));
        meter_->measure(&probe, budget_from_, path_.parent_path());
        budget_view_.status = budget_run_->progress();
    }

    // The run is done: its page goes in as one edit, and the row says what
    // it came to.
    void finish_budget() {
        BudgetOutcome outcome = budget_run_->outcome();
        std::string words = outcome.words;
        if (!std::isfinite(outcome.ms) && !budget_issue_.empty()) {
            words = "Over: " + budget_issue_;
        }
        if (outcome.page) {
            document_ = std::move(*outcome.page);
        }
        if (report_budget_) {
            std::cout << "budget: " << words << std::endl;
        }
        budget_view_ = BudgetView{false, std::move(words), outcome.stop == BudgetStop::Over};
        budget_run_.reset();
        budget_phase_ = BudgetPhase::Idle;
    }

    // ------------------------------------------------------- preview

    Bounds frame_selection_bounds() const {
        if (!document_.scene) { return analysis_ ? analysis_->bounds : Bounds{}; }
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
                radius = std::max(system->radius, 0.1F);
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
        const gui::Rect view{pane.x + 8.0F, pane.y + 8.0F, pane.width - 16.0F, std::max(pane.height - transport_height - 16.0F, 10.0F)};
        preview_aspect_ = view.width / std::max(view.height, 1.0F);

        const rhi::Texture* picture = render_preview(view);
        if (picture != nullptr) {
            ctx_.image(view, *picture);
        } else {
            ctx_.rounded(view, theme.track, theme.radius);
        }
        // Guides, handles and paths over the picture. Their handles take a
        // press before the camera does.
        const Uid working = document_.scene ? scene_view_.selected() : page_.working_layer(document_);
        if (document_.scene && scene_guides_for_ != changes_.value()) {
            scene_guides_ = lower_scene(document_);
            scene_guides_for_ = changes_.value();
        }
        Document& guide_document = document_.scene ? scene_guides_ : document_;
        const SceneItem* selected_item = document_.scene ? document_.scene->item(working) : nullptr;
        const Uid scene_focus = selected_item && std::holds_alternative<Object>(selected_item->data) ? working + 2U : working + 7U;
        GuideOptions guide_options{.focus = document_.scene ? scene_focus : page_.focus(),
                                   .layer = working,
                                   .seconds = static_cast<float>(frame_ / 60.0),
                                   .outlines = &outlines_};
        if (analysis_ && analysis_->bounds.valid) {
            guide_options.bounds = std::pair{analysis_->bounds.minimum, analysis_->bounds.maximum};
        }
        const GuideSet guides = build_guides(guide_document, guide_options);
        const Block* selected_block = document_.scene ? guide_document.block(scene_focus) : nullptr;
        const Value old_position = selected_block ? get(*selected_block, selected_block->kind == BlockKind::Object ? "at" : "offset") : Value{};
        const Value old_size = selected_block ? get(*selected_block, "size") : Value{};
        overlay_.draw(ctx_, view, orbit_, guide_document,
                      OverlayInputs{.guides = &guides,
                                    .analysis = analysis_ ? &*analysis_ : nullptr,
                                    .show_guides = show_guides_,
                                    .show_handles = show_handles_,
                                    .show_paths = show_paths_,
                                    .layer = working,
                                    .frame = frame_});
        if (document_.scene && selected_block) {
            SceneItem* item = document_.scene->item(working);
            const Value position = get(*selected_block, selected_block->kind == BlockKind::Object ? "at" : "offset");
            const Value size = get(*selected_block, "size");
            if (auto* object = item ? std::get_if<Object>(&item->data) : nullptr) {
                if (position != old_position) { object->position = Vec3{position.x, position.y, position.z}; }
                if (size != old_size) { object->size = size.x; }
            } else if (auto* system = item ? std::get_if<ParticleSystem>(&item->data) : nullptr) {
                if (position != old_position) { system->position = Vec3{position.x, position.y, position.z}; }
                if (size != old_size) { system->radius = size.y; }
            }
        }
        ctx_.draw_list().add_corner_mask(view.x, view.y, view.width, view.height, theme.radius, theme.panel);

        // What the picture is, and anything holding it.
        std::string note = document_.scene ? "Initial setup" : "Media server renderer";
        gui::UiColor note_tone = theme.text_muted;
        if (!preview_error_.empty()) {
            note = preview_error_;
            note_tone = theme.warning;
        } else if (preview_ && !preview_->status().issue.empty()) {
            note = preview_->status().issue;
            note_tone = theme.warning;
        } else if (preview_ && preview_->status().waiting) {
            note = "Loading the effect";
            note_tone = theme.caution;
        } else if (!has_package_ && compiled_.effect) {
            note = build_error_.empty() ? "Building the effect" : build_error_;
            note_tone = build_error_.empty() ? theme.caution : theme.warning;
        } else if (bake_quiet_ && builder_.bake_progress() >= 0.0) {
            // Quiet: it never holds anything up, and an edit stops it.
            note = "Baking... " + std::to_string(static_cast<int>(std::lround(builder_.bake_progress() * 100.0))) + "%";
        } else if (show_paths_ && analysis_ && !analysis_->traced) {
            note = "Too many particles to trace their paths";
            note_tone = theme.caution;
        } else if (show_paths_ && !analysis_) {
            note = "Tracing particle paths";
            note_tone = theme.caution;
        } else if (show_paths_ && working == 0U) {
            note = "Open a block to see paths";
            note_tone = theme.caution;
        } else if (show_paths_) {
            const auto count = static_cast<std::size_t>(
                std::count_if(analysis_->paths.begin(), analysis_->paths.end(),
                              [&](const ParticlePath& path) { return path.layer == working; }));
            const Layer* layer = document_.layer(working);
            note = "Paths of " + thousands(count) + " particles" +
                   (layer != nullptr && !layer->name.empty() ? " in " + layer->name : std::string{});
        }
        const float note_w = std::min(ctx_.font_width(note) + 24.0F, view.width - 100.0F);
        const gui::Rect note_rect{view.x + 10.0F, view.y + 10.0F, note_w, 24.0F};
        ctx_.rounded(note_rect, gui::fade(theme.background, 0.7F), note_rect.height * 0.5F);
        ctx_.label(gui::Rect{note_rect.x + 12.0F, note_rect.y, note_rect.width - 16.0F, note_rect.height}, note, note_tone);
        ctx_.tooltip(note_rect, document_.scene ? "Motion edits pause and restart from the initial setup. Color edits preserve motion caches. Preview is simulating; cache playback is not connected yet." : "Drawn by the media server's renderer.");

        // View tools, top right.
        const gui::Rect frame_button{view.right() - 38.0F, view.y + 8.0F, 30.0F, 30.0F};
        if (editor::icon_button(ctx_, gui::ui_id("studio.frame"), frame_button, gui::Icon::Fit, "Frame the effect (F)")) {
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
                                show_handles_ ? "Hide the handles that drag the block you work on (H)"
                                              : "Show handles to drag the block you work on (H)",
                                show_handles_)) {
            show_handles_ = !show_handles_;
        }
        const gui::Rect guides_button{handles_button.x - 34.0F, frame_button.y, 30.0F, 30.0F};
        if (editor::icon_button(ctx_, gui::ui_id("studio.guides"), guides_button, gui::Icon::Guides,
                                show_guides_ ? "Hide guides (G)" : "Show guides (G)",
                                show_guides_)) {
            show_guides_ = !show_guides_;
        }
        // The sandbox tools down the left, under the note. The die deals the
        // layer being worked on, or the whole page, another seed.
        if (!document_.scene) { static_cast<void>(sandbox_.draw_strip(ctx_, view.x + 10.0F, view.y + 44.0F, document_, working)); }
        // After the tools over the picture: the first control a press lands
        // on takes it, and the camera covers the whole view.
        camera_input(view);

        // What it costs, bottom left.
        std::string cost = thousands(compiled_.total_capacity()) + " particles at most";
        if (gpu_ms_ > 0.0) {
            cost = "GPU " + format_number(static_cast<float>(gpu_ms_), 2) + " ms a frame, " + cost;
        }
        const float cost_w = std::min(ctx_.font_width(cost) + 24.0F, view.width - 20.0F);
        const gui::Rect cost_rect{view.x + 10.0F, view.bottom() - 34.0F, cost_w, 24.0F};
        ctx_.rounded(cost_rect, gui::fade(theme.background, 0.7F), cost_rect.height * 0.5F);
        ctx_.label(gui::Rect{cost_rect.x + 12.0F, cost_rect.y, cost_rect.width - 16.0F, cost_rect.height}, cost,
                   theme.text_muted);
        ctx_.tooltip(cost_rect, "GPU time for the whole preview frame.");

        draw_transport(gui::Rect{pane.x + 8.0F, view.bottom() + 8.0F, pane.width - 16.0F, transport_height - 8.0F});
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
        sandbox_click(view, hit);
        if (hit.clicked && sandbox_.tool() == SandboxTool::Select && clicks_.double_click(1U, ctx_.time_seconds())) {
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

    // A click in the preview with the sandbox tool that is on: a solid thing
    // picked, or an event written into the page at the playhead, against
    // where the solid things stand now (the analysis's samples).
    void sandbox_click(const gui::Rect& view, const gui::Interaction& hit) {
        const StageBodies* standing =
            analysis_ ? stage_bodies_at(analysis_->bodies, static_cast<std::uint64_t>(frame_)) : nullptr;
        if (document_.scene) {
            if (hit.clicked && standing) {
                const StageCamera camera{camera_pose(orbit_), view};
                Vec3 origin{}, direction{};
                camera.ray(ctx_.pointer_x(), ctx_.pointer_y(), origin, direction);
                if (const auto picked = pick_stage(standing->bodies, origin, direction, 0.0F); picked && picked->on_body) {
                    scene_view_.select(picked->layer, ctx_.ctrl_held());
                }
            }
            return;
        }
        SandboxScene scene;
        if (standing != nullptr) {
            scene.bodies = std::span<const StageBody>{standing->bodies};
        }
        scene.floor = stage_floor(document_);
        // The show frame the playhead is on, which is the step the event
        // happens in.
        scene.seconds = static_cast<float>(std::floor(frame_) / 60.0);
        scene.layer = page_.target_layer(document_);
        scene.selected = page_.focus();
        const StageCamera camera{camera_pose(orbit_), view};
        const std::optional<SandboxResult> result = sandbox_.use_click(ctx_, hit, camera, document_, scene);
        if (!result) {
            return;
        }
        if (result->block != 0U) {
            page_.select_block(document_, result->block);
        } else if (result->layer != 0U) {
            page_.select_layer(result->layer);
        }
        if (result->tool != SandboxTool::Select) {
            // Seen at once: paused where it was placed.
            playing_ = false;
        }
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
        frame.staging_floor = !document_.scene;
        frame.orbit = orbit_;
        frame.package = has_package_ ? builder_.preview_path().string() : std::string{};
        frame.package_generation = package_generation_;
        frame.frame = static_cast<std::uint64_t>(frame_);
        frame.revision = package_generation_;
        frame.paused = !playing_;
        // Keys sorted, as a placement keeps them: "name.component".
        for (const auto& [name, value] : page_.preview_values()) {
            for (const ParameterDesc& parameter : compiled_.effect ? compiled_.effect->parameters : std::vector<ParameterDesc>{}) {
                if (parameter.name != name) {
                    continue;
                }
                const std::size_t components = parameter.type == ParameterType::Float ? 1U
                                               : parameter.type == ParameterType::Vec3 ? 3U
                                                                                        : 4U;
                for (std::size_t i = 0U; i < components; ++i) {
                    mediaserver::scene::ParticleParameter entry;
                    entry.key = name + "." + std::to_string(i);
                    entry.base = value[i];
                    frame.parameters.push_back(std::move(entry));
                }
            }
        }
        std::sort(frame.parameters.begin(), frame.parameters.end(),
                  [](const auto& a, const auto& b) { return a.key < b.key; });
        // The budget's meter draws at this size, from this camera.
        budget_look_ = frame;
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
        const gui::Rect track{bar.x + 150.0F, bar.center_y() - 10.0F, std::max(bar.width - 150.0F - time_w - 12.0F, 20.0F), 20.0F};
        draw_events(track, frames);
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
        // it holds the baked layers as they are, amber once it is stale.
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

    // The page's events over the scrub bar, a diamond each where it happens:
    // explosions, pushes and things that appear. A click opens its block and
    // takes the playhead there. Taken before the scrub bar, so a press on one
    // is its own.
    void draw_events(const gui::Rect& track, double frames) {
        if (events_for_ != changes_.value() || events_dirty_) {
            events_ = page_events(document_);
            events_for_ = changes_.value();
            events_dirty_ = false;
        }
        const gui::Theme& theme = ctx_.theme();
        for (const PageEvent& event : events_) {
            const float u = std::clamp(static_cast<float>(static_cast<double>(event.seconds) * 60.0 / frames), 0.0F, 1.0F);
            const gui::UiPoint at{track.x + track.width * u, track.y - 6.0F};
            const gui::Rect hit_rect{at.x - 6.0F, at.y - 6.0F, 12.0F, 12.0F};
            const gui::Interaction hit = ctx_.grab(part_id(event.block, "studio.event"), hit_rect);
            ctx_.cursor_for(hit, gui::Cursor::Hand);
            const float lit = ctx_.ease(part_id(event.block, "studio.event.hover"),
                                        hit.hovered || page_.focus() == event.block ? 1.0F : 0.0F);
            gui::add_diamond(ctx_.draw_list(), at, 4.0F + lit, gui::blend(theme.text_muted, theme.accent, lit));
            ctx_.tooltip(hit_rect, event.label);
            if (hit.clicked) {
                page_.select_block(document_, event.block);
                frame_ = std::min(static_cast<double>(event.seconds) * 60.0, frames - 1.0);
                playing_ = false;
            }
        }
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
    PageView page_;
    SceneView scene_view_;
    float scene_tree_split_{0.19F};
    float scene_inspector_split_{0.66F};
    Document scene_guides_;
    std::uint64_t scene_guides_for_{};
    Document scene_motion_;
    std::uint64_t scene_motion_for_{};
    StageOverlay overlay_;
    SandboxTools sandbox_;
    // The page's events the transport marks, and the page they were read from.
    std::vector<PageEvent> events_;
    std::uint64_t events_for_{0U};
    bool events_dirty_{true};
    Palette palette_;
    SessionState session_;
    bool show_guides_{true};
    bool show_handles_{true};
    bool show_paths_{false};
    std::string asset_issue_;
    std::map<std::string, std::vector<Vec3>, std::less<>> outlines_;
    std::uint32_t settled_frames_{0U};
    PopoverList gallery_;
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
    // The coach's notes, and what they were worked out from: the page, its
    // compile and its bake status.
    std::vector<Advice> advice_;
    std::array<std::uint64_t, 3> advice_key_{};
    std::vector<Document> history_;
    std::size_t history_index_{0U};
    std::filesystem::path path_;
    std::uint64_t revision_{0U};
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
    bool expert_{false};
    float split_{0.46F};
    std::array<Flight, 2> flights_{};
    const rhi::Texture* last_picture_{nullptr};
    double gpu_ms_{0.0};
    // "Maximize budget": its meter (a second preview, made when first
    // asked), where a run is, and what the page's budget row shows.
    enum class BudgetPhase : std::uint8_t {
        Idle,
        Limiting, // working out how far the page may scale, off this thread
        Stage,    // measuring the stage alone
        Probing,  // measuring the page at the factors the search asks for
    };
    std::unique_ptr<BudgetMeter> meter_;
    BudgetPhase budget_phase_{BudgetPhase::Idle};
    std::future<DensityLimit> budget_limit_;
    std::optional<BudgetRun> budget_run_;
    DensityOptions budget_options_;
    bool budget_down_{false};
    std::uint64_t budget_for_{0U};
    std::uint64_t budget_from_{0U};
    double budget_stage_ms_{0.0};
    std::string budget_issue_;
    PreviewFrame budget_look_;
    BudgetView budget_view_;
    // --maximize: press it once the effect is up, and say what was measured.
    bool maximize_at_start_{false};
    bool report_budget_{false};
    Toast toast_;
    bool open_picker_{false};
    bool night_{false};
    double lose_device_at_{-1.0};
    std::uint64_t frames_{0U};
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
