#include "studio_app.hpp"

#include "coach.hpp"
#include "compile.hpp"
#include "document_io.hpp"
#include "package_builder.hpp"
#include "page_view.hpp"
#include "palette.hpp"
#include "preview.hpp"
#include "recipes.hpp"
#include "session.hpp"
#include "stage_overlay.hpp"
#include "widgets.hpp"

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/input.hpp>
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
#include <iostream>
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
        looping_ = session_.looping;

        std::optional<Document> recipe = make_recipe(options.recipe);
        if (!recipe) {
            recipe = make_recipe("campfire");
        }
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
        return true;
    }

    // One frame. Returns false once the window should close.
    bool frame() {
        if (!window_.pump(input_)) {
            const bool changed = !(document_ == saved_);
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
        ctx_.new_frame(now);

        take_results(now);
        advance_playback(dt);
        lose_device_if_asked(now);

        const float width = static_cast<float>(window_.width());
        const float height = static_cast<float>(window_.height());
        const gui::Rect window{0.0F, 0.0F, width, height};
        const gui::Theme& theme = ctx_.theme();
        ctx_.panel(window, theme.background);

        draw_top_bar(gui::Rect{0.0F, 0.0F, width, top_bar_height});
        const gui::Rect body{10.0F, top_bar_height, std::max(width - 20.0F, 10.0F), std::max(height - top_bar_height - 10.0F, 10.0F)};
        // The palette down the left, then the page and the preview.
        const float palette_w = palette_.is_open() ? Palette::open_width : Palette::rail_width;
        const gui::Rect palette_rect{body.x, body.y, palette_w, body.height};
        const gui::Rect rest{body.x + palette_w + 10.0F, body.y, std::max(body.width - palette_w - 10.0F, 10.0F),
                             body.height};
        const gui::Split split = ctx_.split(gui::ui_id("studio.split"), rest, split_, gui::Axis::Row, 10.0F);

        const std::vector<Advice> advice = advise(document_, compiled_);
        PageInputs inputs{&compiled_, &advice, analysis_ ? &*analysis_ : nullptr, expert_,
                          natural_loop_seconds(compiled_)};
        palette_.draw(ctx_, palette_rect, document_, page_.working_layer(document_));
        page_.set_incoming(palette_.held());
        page_.draw(ctx_, split.first, document_, inputs);
        if (std::optional<Incoming> clicked = palette_.take_clicked()) {
            page_.add(document_, *clicked);
        }
        draw_preview(split.second, now);
        page_.draw_overlays(ctx_, window, document_, inputs);
        palette_.end_frame(ctx_);
        page_.set_incoming(nullptr);
        draw_gallery(window);
        draw_toast(window, now);
        ctx_.draw_overlays(window);

        if (open_picker_ && frames_ > 2U) {
            page_.open_block_picker(document_);
            open_picker_ = false;
        }
        ++frames_;
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

    void replace_document(Document document, std::filesystem::path path) {
        document_ = std::move(document);
        path_ = std::move(path);
        saved_ = document_;
        history_.assign(1U, document_);
        history_index_ = 0U;
        page_.forget();
        compiled_ = compile(document_, Purpose::Preview);
        compiled_for_ = document_;
        analysis_.reset();
        want_frame_ = true;
        frame_ = 0.0;
        playing_ = true;
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
        if (!discarded && (path_.empty() || !(document_ == saved_))) {
            session.unsaved = save_document(document_);
        }
        session.view = page_.view_state();
        remember_view(session, session.page, session.view);
        session.width = window_.width();
        session.height = window_.height();
        session.split = split_;
        session.palette = palette_.is_open();
        session.palette_folded = palette_.folded();
        session.guides = show_guides_;
        session.handles = show_handles_;
        session.paths = show_paths_;
        session.figure = figure_;
        session.expert = expert_;
        session.looping = looping_;
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
        if (document_ == saved_) {
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

    bool confirm_discard(const char* action) {
        if (document_ == saved_) {
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
        saved_ = document_;
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
        builder_.submit_export(document_, path, path_.parent_path());
        toast("Exporting " + path.filename().string() + "...", true);
    }

    void toast(std::string text, bool ok) {
        toast_ = Toast{std::move(text), ok, window_.time_seconds() + (ok ? 4.0 : 8.0)};
    }

    // ------------------------------------------------------- the builder

    void submit(double now) {
        ++revision_;
        builder_.submit(document_, revision_, path_.parent_path());
        submitted_ = document_;
        last_submit_ = now;
    }

    void take_results(double now) {
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
            } else if (!built->compiled.effect) {
                has_package_ = false;
            }
        }
        if (std::optional<Analysis> analysis = builder_.take_analysis()) {
            analysis_ = std::move(analysis);
            if (want_frame_) {
                orbit_ = frame_bounds(orbit_, analysis_->bounds, preview_aspect_);
                want_frame_ = false;
            }
        }
        if (std::optional<ExportResult> exported = builder_.take_export()) {
            toast(exported->message, exported->ok);
        }
        static_cast<void>(now);
    }

    void follow_document(double now) {
        if (!(document_ == compiled_for_)) {
            compiled_ = compile(document_, Purpose::Preview);
            compiled_for_ = document_;
        }
        if (preview_ && !preview_->status().waiting && has_package_) {
            ++settled_frames_;
        }
        if (!(document_ == submitted_)) {
            const bool live = page_.gesture_live() || ctx_.pointer_down() || overlay_.dragging();
            // While a value is dragged, the next package waits for the preview
            // to have taken in the last; the gesture's end always sends one.
            const bool settled = !builder_.busy() && (settled_frames_ >= settle_frames || !preview_ ||
                                                      now - last_submit_ >= settle_timeout);
            if (!live || (now - last_submit_ >= submit_interval && settled)) {
                submit(now);
            }
        }
    }

    void record_history() {
        const bool gesture = page_.gesture_live() || ctx_.pointer_down() || ctx_.focus() != gui::UiId{} ||
                             ctx_.pointer_blocked();
        if (gesture || document_ == history_[history_index_]) {
            return;
        }
        history_.resize(history_index_ + 1U);
        history_.push_back(document_);
        if (history_.size() > history_limit) {
            history_.erase(history_.begin());
        }
        history_index_ = history_.size() - 1U;
    }

    void undo() {
        if (history_index_ > 0U) {
            document_ = history_[--history_index_];
        }
    }

    void redo() {
        if (history_index_ + 1U < history_.size()) {
            document_ = history_[++history_index_];
        }
    }

    void shortcuts() {
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
            page_.open_block_picker(document_);
        }
        if (typed.find('f') != std::string_view::npos || typed.find('F') != std::string_view::npos) {
            orbit_ = frame_bounds(orbit_, analysis_ ? analysis_->bounds : Bounds{}, preview_aspect_);
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
            if (frame_ >= frames) {
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
        frame_ = std::clamp(frame_, 0.0, std::nextafter(frames, 0.0));
    }

    void toggle_playing() {
        // Play at the end of a run that does not loop starts it again.
        if (!playing_ && !looping_ && frame_ >= loop_frames() - 1.0) {
            frame_ = 0.0;
        }
        playing_ = !playing_;
    }

    // ------------------------------------------------------- top bar

    void draw_top_bar(const gui::Rect& bar) {
        const gui::Theme& theme = ctx_.theme();
        float x = bar.x + 18.0F;
        label_sized(ctx_, gui::Rect{x, bar.y, 60.0F, bar.height}, "ajfx", theme.text, 1.2F);
        x += text_width_sized(ctx_, "ajfx", 1.2F) + 6.0F;
        ctx_.label(gui::Rect{x, bar.y + 2.0F, 60.0F, bar.height}, "Studio", theme.text_muted);
        x += 70.0F;

        const float y = bar.y + 11.0F;
        if (editor::pill_button(ctx_, gui::ui_id("studio.new"), gui::Rect{x, y, 84.0F, 30.0F}, gui::Icon::Plus, "New",
                                "Start a new effect from a recipe")) {
            open_gallery();
        }
        x += 90.0F;
        if (editor::pill_button(ctx_, gui::ui_id("studio.open"), gui::Rect{x, y, 84.0F, 30.0F}, gui::Icon::Import,
                                "Open", "Open an ajfx Studio page")) {
            open_dialog();
        }
        x += 90.0F;
        if (editor::pill_button(ctx_, gui::ui_id("studio.save"), gui::Rect{x, y, 84.0F, 30.0F}, std::nullopt, "Save",
                                "Save the page (Ctrl+S)")) {
            static_cast<void>(save(false));
        }
        x += 90.0F;
        if (editor::pill_button(ctx_, gui::ui_id("studio.export"), gui::Rect{x, y, 150.0F, 30.0F}, gui::Icon::Publish,
                                "Export .ajfx", "Export a package the media server plays, kernels included",
                                editor::ButtonTone::Primary)) {
            export_dialog();
        }
        x += 166.0F;

        // The page's file, and whether it has unsaved changes.
        std::string file = path_.empty() ? std::string{"Not saved yet"} : path_.filename().string();
        const bool dirty = !(document_ == saved_);
        const gui::Rect file_rect{x, bar.y, std::max(bar.right() - x - 330.0F, 40.0F), bar.height};
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
        if (editor::icon_button(ctx_, gui::ui_id("studio.expert"), gui::Rect{rx, y + 1.0F, 28.0F, 28.0F},
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
        } else if (builder_.busy() || !(document_ == submitted_)) {
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
            tip = asset_issue_ + ". The preview plays without it; export waits until it can be read.";
        } else if (kernel_ms_ > 0.0) {
            tip += " Its kernels compiled in " + format_number(static_cast<float>(kernel_ms_), 0) + " ms.";
        }
        const float state_w = ctx_.font_width(state) + 28.0F;
        const gui::Rect chip{rx - state_w - 12.0F, y + 2.0F, state_w, 26.0F};
        ctx_.rounded(chip, theme.track, chip.height * 0.5F);
        ctx_.rounded(gui::Rect{chip.x + 10.0F, chip.center_y() - 3.5F, 7.0F, 7.0F}, tone, 3.5F);
        ctx_.label(gui::Rect{chip.x + 22.0F, chip.y, chip.width - 22.0F, chip.height}, state, theme.text);
        ctx_.tooltip(chip, tip);
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

    // ------------------------------------------------------- preview

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
        const Uid working = page_.working_layer(document_);
        GuideOptions guide_options{.focus = page_.focus(),
                                   .layer = working,
                                   .seconds = static_cast<float>(frame_ / 60.0),
                                   .outlines = &outlines_};
        if (analysis_ && analysis_->bounds.valid) {
            guide_options.bounds = std::pair{analysis_->bounds.minimum, analysis_->bounds.maximum};
        }
        const GuideSet guides = build_guides(document_, guide_options);
        overlay_.draw(ctx_, view, orbit_, document_,
                      OverlayInputs{.guides = &guides,
                                    .analysis = analysis_ ? &*analysis_ : nullptr,
                                    .show_guides = show_guides_,
                                    .show_handles = show_handles_,
                                    .show_paths = show_paths_,
                                    .layer = working,
                                    .frame = frame_});
        ctx_.draw_list().add_corner_mask(view.x, view.y, view.width, view.height, theme.radius, theme.panel);

        // What the picture is, and anything holding it.
        std::string note = "Media server renderer";
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
        } else if (show_paths_ && analysis_ && !analysis_->traced) {
            note = "Too many particles to trace their paths";
            note_tone = theme.caution;
        } else if (show_paths_ && !analysis_) {
            note = "Tracing particle paths";
            note_tone = theme.caution;
        } else if (show_paths_ && working == 0U) {
            note = "Paths show for the layer you work on: open one of its blocks";
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
        ctx_.tooltip(note_rect, "The preview is drawn by the media server's Advanced renderer, so it matches the wall.");

        // View tools, top right.
        const gui::Rect frame_button{view.right() - 38.0F, view.y + 8.0F, 30.0F, 30.0F};
        if (editor::icon_button(ctx_, gui::ui_id("studio.frame"), frame_button, gui::Icon::Fit, "Frame the effect (F)")) {
            orbit_ = frame_bounds(orbit_, analysis_ ? analysis_->bounds : Bounds{}, preview_aspect_);
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
                                show_guides_ ? "Hide guides: shapes, pushes, turbulence and colliders (G)"
                                             : "Show where the layer you work on acts: shapes, pushes, turbulence "
                                               "and colliders (G)",
                                show_guides_)) {
            show_guides_ = !show_guides_;
        }
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
        ctx_.tooltip(cost_rect, "GPU time is the whole preview frame - stage, particles and post - as the GPU measured it.");

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
        if (hit.clicked && clicks_.double_click(1U, ctx_.time_seconds())) {
            orbit_ = frame_bounds(orbit_, analysis_ ? analysis_->bounds : Bounds{}, preview_aspect_);
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
        const gui::Interaction hit = ctx_.grab(gui::ui_id("studio.scrub"), track.inflated(0.0F, 6.0F));
        ctx_.cursor_for(hit, gui::Cursor::ResizeHorizontal);
        ctx_.rounded(gui::Rect{track.x, track.center_y() - 3.0F, track.width, 6.0F}, theme.track, 3.0F);
        const float t = static_cast<float>(frame_ / frames);
        ctx_.rounded(gui::Rect{track.x, track.center_y() - 3.0F, track.width * t, 6.0F}, gui::fade(theme.playhead, 0.35F), 3.0F);
        const int seconds = static_cast<int>(frames / 60.0);
        for (int s = 1; s < seconds; ++s) {
            const float tx = track.x + track.width * static_cast<float>(s * 60.0 / frames);
            ctx_.panel(gui::Rect{tx, track.center_y() + 5.0F, 1.0F, 5.0F}, theme.text_muted);
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
    PageView page_;
    StageOverlay overlay_;
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
    Document saved_;
    Document submitted_;
    Document compiled_for_;
    Compiled compiled_;
    std::vector<Document> history_;
    std::size_t history_index_{0U};
    std::filesystem::path path_;
    std::uint64_t revision_{0U};
    double last_submit_{0.0};
    std::string build_error_;
    double kernel_ms_{0.0};
    std::optional<Analysis> analysis_;

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
    Toast toast_;
    bool open_picker_{false};
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
