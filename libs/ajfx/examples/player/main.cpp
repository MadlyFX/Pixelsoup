// ajfx_player: a small media player for .ajfx effects.
//
//   ajfx_player [file.ajfx]
//   ajfx_player --register | --unregister
//   ajfx_player file.ajfx --screenshot out.png [--seconds 2]
//
// Open a file by double-clicking it (once .ajfx is associated), by dropping
// it on the window, or with File > Open. The status bar shows what to check:
// live particles per emitter, the step, the time a frame takes, and whether
// the package's precompiled kernels were used or why not.
//
// --register associates .ajfx files with this executable for the current
// Windows user only (HKEY_CURRENT_USER\Software\Classes); --unregister
// removes that again. --screenshot renders one frame offscreen and exits.

#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include "../common/playback.hpp"
#include "../viewer/png.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "  \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")

namespace {

using Clock = std::chrono::steady_clock;

enum Command : int {
    open_file = 101,
    reload,
    preset_sparks,
    preset_smoke,
    preset_confetti,
    associate,
    unassociate,
    exit_app,
    play_pause,
    restart,
    back_second,
    forward_second,
    back_step,
    forward_step,
    toggle_loop,
    frame_effect,
    // Controls
    play_button = 201,
    timeline,
    length_box,
};

constexpr double lengths[] = {5.0, 10.0, 20.0, 30.0, 60.0};
constexpr int timeline_resolution = 1000;
const wchar_t* const prog_id = L"ajfx.Effect";

std::wstring widen(const std::string& text) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::wstring grouped(std::uint64_t value) {
    std::wstring digits = std::to_wstring(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), L",");
    }
    return digits;
}

std::wstring executable_path() {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2U);
    }
}

// ------------------------------------------------------------ file association

bool set_default_value(const std::wstring& key, const std::wstring& value) {
    HKEY handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status =
        RegSetValueExW(handle, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t)));
    RegCloseKey(handle);
    return status == ERROR_SUCCESS;
}

std::optional<std::wstring> default_value(const std::wstring& key) {
    wchar_t value[256]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return std::wstring{value};
}

// Per-user only: nothing outside HKEY_CURRENT_USER\Software\Classes changes.
bool register_association() {
    const std::wstring command = L"\"" + executable_path() + L"\" \"%1\"";
    const std::wstring classes = L"Software\\Classes\\";
    const bool ok = set_default_value(classes + L".ajfx", prog_id) &&
                    set_default_value(classes + prog_id, L"ajfx effect") &&
                    set_default_value(classes + prog_id + L"\\shell\\open\\command", command);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool unregister_association() {
    const std::wstring classes = L"Software\\Classes\\";
    RegDeleteTreeW(HKEY_CURRENT_USER, (classes + prog_id).c_str());
    // Only take back .ajfx if it still points at this player's class.
    if (default_value(classes + L".ajfx") == std::optional<std::wstring>{prog_id}) {
        RegDeleteTreeW(HKEY_CURRENT_USER, (classes + L".ajfx").c_str());
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return !default_value(classes + prog_id + L"\\shell\\open\\command").has_value();
}

// ------------------------------------------------------------ the application

struct App final {
    std::unique_ptr<ajfx::d3d12::Device> device;
    std::unique_ptr<ajfx::gpu::ShaderCompiler> compiler;
    std::unique_ptr<samples::Player> player;
    std::wstring path; // empty for a preset
    std::wstring name;

    HWND window{nullptr};
    HWND view{nullptr};
    HWND play{nullptr};
    HWND slider{nullptr};
    HWND clock{nullptr};
    HWND length{nullptr};
    HWND status{nullptr};
    HMENU menu{nullptr};
    HFONT font{nullptr};
    HACCEL accelerators{nullptr};

    ajfx::gpu::Texture target;
    std::uint32_t width{0U};
    std::uint32_t height{0U};

    samples::Orbit orbit;
    bool dragging{false};
    POINT last{};

    bool playing{true};
    bool looping{true};
    bool scrubbing{false};
    double time{0.0};
    double duration{10.0};
    double frame_ms{0.0};
    Clock::time_point previous{Clock::now()};
    Clock::time_point status_due{Clock::now()};
};

App g;

void show_error(const std::wstring& title, const std::string& message) {
    MessageBoxW(g.window, widen(message).c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}

std::uint64_t current_step() {
    if (!g.player) {
        return 0U;
    }
    return static_cast<std::uint64_t>(std::floor(g.time * g.player->effect().simulation_rate + 1.0e-9));
}

void update_title() {
    const std::wstring shown = g.name.empty() ? L"no effect" : g.name;
    SetWindowTextW(g.window, (shown + L" - ajfx player").c_str());
}

void update_controls() {
    SetWindowTextW(g.play, g.playing ? L"Pause" : L"Play");
    CheckMenuItem(g.menu, toggle_loop, g.looping ? MF_CHECKED : MF_UNCHECKED);
    if (!g.scrubbing) {
        SendMessageW(g.slider, TBM_SETPOS, TRUE,
                     static_cast<LPARAM>(std::lround(g.time / g.duration * timeline_resolution)));
    }
    wchar_t text[64];
    swprintf(text, std::size(text), L"%.2f / %.0f s", g.time, g.duration);
    SetWindowTextW(g.clock, text);
}

void update_status() {
    if (!g.player) {
        SendMessageW(g.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"Open an .ajfx file, or drop one here"));
        for (WPARAM part = 1; part < 3; ++part) {
            SendMessageW(g.status, SB_SETTEXTW, part, reinterpret_cast<LPARAM>(L""));
        }
        return;
    }
    const std::vector<std::uint32_t> counts = g.player->alive_counts(*g.device);
    std::uint64_t total = 0U;
    std::wstring detail;
    for (std::size_t i = 0U; i < counts.size(); ++i) {
        total += counts[i];
        detail += (i == 0U ? L" (" : L", ") + widen(g.player->effect().emitters[i].name) + L" " + grouped(counts[i]);
    }
    const std::wstring particles = grouped(total) + L" particles" + (counts.size() > 1U ? detail + L")" : L"");
    wchar_t timing[96];
    swprintf(timing, std::size(timing), L"Step %llu  |  %.2f ms a frame",
             static_cast<unsigned long long>(g.player->simulation().tick()), g.frame_ms);
    const std::wstring kernels = widen(g.player->kernel_note()) + L"  |  " + widen(std::string{g.device->adapter_name()});
    SendMessageW(g.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(particles.c_str()));
    SendMessageW(g.status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(timing));
    SendMessageW(g.status, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(kernels.c_str()));
}

void frame_view() {
    if (!g.player) {
        return;
    }
    // Everywhere the effect reaches during the loop.
    const auto box = g.player->bounds_until(g.duration);
    if (box) {
        g.orbit.frame(*box);
    }
    g.player->seek(current_step());
}

bool load(ajfx::Effect effect, const std::vector<std::uint8_t>* package, std::wstring path, std::wstring name) {
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514))); // wait
    ajfx::Result<std::unique_ptr<samples::Player>> player =
        samples::Player::create(*g.device, g.compiler.get(), std::move(effect), package);
    if (!player) {
        show_error(L"Cannot play " + name, player.error().message);
        return false;
    }
    g.player = std::move(player).value();
    g.path = std::move(path);
    g.name = std::move(name);
    g.time = 0.0;
    g.playing = true;
    frame_view();
    update_title();
    update_controls();
    update_status();
    return true;
}

bool open(const std::wstring& path) {
    const std::optional<std::vector<std::uint8_t>> bytes = samples::read_file(path);
    const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1U);
    if (!bytes) {
        show_error(L"Cannot open " + name, "The file could not be read.");
        return false;
    }
    ajfx::Result<ajfx::Effect> effect = ajfx::read_package(*bytes);
    if (!effect) {
        show_error(L"Cannot open " + name, effect.error().message);
        return false;
    }
    return load(std::move(effect).value(), &*bytes, path, name);
}

void open_preset(const char* preset) {
    load(*samples::preset(preset), nullptr, L"", widen(preset) + L" (preset)");
}

void choose_file() {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g.window;
    dialog.lpstrFilter = L"ajfx effects (*.ajfx)\0*.ajfx\0All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog)) {
        open(file);
    }
}

void seek_to(double seconds) {
    g.time = std::clamp(seconds, 0.0, g.duration);
    update_controls();
}

void command(int id) {
    const double step = g.player ? 1.0 / g.player->effect().simulation_rate : 1.0 / 60.0;
    switch (id) {
    case open_file:
        choose_file();
        break;
    case reload:
        if (!g.path.empty()) {
            open(g.path);
        }
        break;
    case preset_sparks:
        open_preset("sparks");
        break;
    case preset_smoke:
        open_preset("smoke");
        break;
    case preset_confetti:
        open_preset("confetti");
        break;
    case associate:
        if (MessageBoxW(g.window,
                        (L"Open .ajfx files with this player when they are double-clicked?\n\n" + executable_path() +
                         L"\n\nThis changes only your own Windows user's file types, and File > Remove "
                         L"the association undoes it.")
                            .c_str(),
                        L"Associate .ajfx files", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            MessageBoxW(g.window,
                        register_association() ? L".ajfx files now open in this player."
                                               : L"The association could not be written.",
                        L"Associate .ajfx files", MB_OK);
        }
        break;
    case unassociate:
        MessageBoxW(g.window,
                    unregister_association() ? L"The association has been removed."
                                             : L"The association could not be removed.",
                    L"Remove the association", MB_OK);
        break;
    case exit_app:
        DestroyWindow(g.window);
        break;
    case play_pause:
    case play_button:
        g.playing = !g.playing;
        if (g.playing && g.time >= g.duration) {
            g.time = 0.0;
        }
        break;
    case restart:
        seek_to(0.0);
        break;
    case back_second:
        seek_to(g.time - 1.0);
        break;
    case forward_second:
        seek_to(g.time + 1.0);
        break;
    case back_step:
        g.playing = false;
        seek_to(static_cast<double>(current_step() > 0U ? current_step() - 1U : 0U) * step);
        break;
    case forward_step:
        g.playing = false;
        seek_to(static_cast<double>(current_step() + 1U) * step);
        break;
    case toggle_loop:
        g.looping = !g.looping;
        break;
    case frame_effect:
        frame_view();
        break;
    default:
        break;
    }
    update_controls();
}

// ------------------------------------------------------------ layout

int scaled(int pixels) {
    return MulDiv(pixels, static_cast<int>(GetDpiForWindow(g.window)), 96);
}

void apply_font() {
    if (g.font != nullptr) {
        DeleteObject(g.font);
    }
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, GetDpiForWindow(g.window));
    g.font = CreateFontIndirectW(&metrics.lfMessageFont);
    for (HWND control : {g.play, g.slider, g.clock, g.length, g.status}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    }
}

void layout() {
    RECT client{};
    GetClientRect(g.window, &client);
    SendMessageW(g.status, WM_SIZE, 0, 0);
    RECT status{};
    GetWindowRect(g.status, &status);
    const int status_height = status.bottom - status.top;
    const int bar = scaled(40);
    const int pad = scaled(6);
    const int width = client.right;
    const int bar_top = client.bottom - status_height - bar;

    MoveWindow(g.view, 0, 0, width, std::max(bar_top, 1), TRUE);
    const int control = bar - 2 * pad;
    const int play_width = scaled(72);
    const int clock_width = scaled(110);
    const int length_width = scaled(80);
    MoveWindow(g.play, pad, bar_top + pad, play_width, control, TRUE);
    const int slider_left = 2 * pad + play_width;
    const int slider_width = std::max(width - slider_left - clock_width - length_width - 3 * pad, 10);
    MoveWindow(g.slider, slider_left, bar_top + pad, slider_width, control, TRUE);
    MoveWindow(g.clock, slider_left + slider_width + pad, bar_top + pad + scaled(4), clock_width, control, TRUE);
    MoveWindow(g.length, width - pad - length_width, bar_top + pad, length_width, scaled(200), TRUE);

    const int parts[] = {width / 3, (width * 5) / 9, -1};
    SendMessageW(g.status, SB_SETPARTS, std::size(parts), reinterpret_cast<LPARAM>(parts));
}

// ------------------------------------------------------------ window procedures

LRESULT CALLBACK view_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_LBUTTONDOWN:
        g.dragging = true;
        g.last = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
        SetFocus(g.window);
        return 0;
    case WM_LBUTTONUP:
        g.dragging = false;
        ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK:
        frame_view();
        return 0;
    case WM_MOUSEMOVE:
        if (g.dragging) {
            const POINT now{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            g.orbit.yaw -= static_cast<float>(now.x - g.last.x) * 0.01F;
            g.orbit.pitch = std::clamp(g.orbit.pitch + static_cast<float>(now.y - g.last.y) * 0.01F, -1.5F, 1.5F);
            g.last = now;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        layout();
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        apply_font();
        layout();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == length_box && HIWORD(wparam) == CBN_SELCHANGE) {
            const LRESULT index = SendMessageW(g.length, CB_GETCURSEL, 0, 0);
            g.duration = lengths[std::clamp<LRESULT>(index, 0, static_cast<LRESULT>(std::size(lengths)) - 1)];
            seek_to(g.time);
            SetFocus(hwnd);
            return 0;
        }
        command(LOWORD(wparam));
        if (LOWORD(wparam) == play_button) {
            SetFocus(hwnd); // keep Space for the accelerator
        }
        return 0;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) == g.slider) {
            const auto position = static_cast<double>(SendMessageW(g.slider, TBM_GETPOS, 0, 0));
            g.scrubbing = LOWORD(wparam) == TB_THUMBTRACK;
            g.time = position / timeline_resolution * g.duration;
            update_controls();
        }
        return 0;
    case WM_MOUSEWHEEL:
        g.orbit.distance = std::clamp(g.orbit.distance * (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 0.9F : 1.1F), 0.2F, 500.0F);
        return 0;
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wparam);
        wchar_t file[MAX_PATH]{};
        if (DragQueryFileW(drop, 0U, file, MAX_PATH) > 0U) {
            open(file);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

HMENU build_menu() {
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, open_file, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, reload, L"&Reload\tF5");
    HMENU presets = CreatePopupMenu();
    AppendMenuW(presets, MF_STRING, preset_sparks, L"&Sparks");
    AppendMenuW(presets, MF_STRING, preset_smoke, L"S&moke");
    AppendMenuW(presets, MF_STRING, preset_confetti, L"&Confetti");
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(presets), L"&Presets");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, associate, L"&Associate .ajfx files with this player...");
    AppendMenuW(file, MF_STRING, unassociate, L"Remove the &association");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, exit_app, L"E&xit");

    HMENU playback = CreatePopupMenu();
    AppendMenuW(playback, MF_STRING, play_pause, L"&Play or pause\tSpace");
    AppendMenuW(playback, MF_STRING, restart, L"&Restart\tHome");
    AppendMenuW(playback, MF_STRING, back_second, L"&Back a second\tLeft");
    AppendMenuW(playback, MF_STRING, forward_second, L"&Forward a second\tRight");
    AppendMenuW(playback, MF_STRING, back_step, L"Previous &step\t,");
    AppendMenuW(playback, MF_STRING, forward_step, L"&Next step\t.");
    AppendMenuW(playback, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playback, MF_STRING | MF_CHECKED, toggle_loop, L"&Loop\tL");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, frame_effect, L"&Frame the effect\tF");

    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(playback), L"&Playback");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    return bar;
}

HACCEL build_accelerators() {
    ACCEL keys[] = {
        {FVIRTKEY | FCONTROL, 'O', open_file}, {FVIRTKEY, VK_F5, reload},
        {FVIRTKEY, VK_SPACE, play_pause},      {FVIRTKEY, VK_HOME, restart},
        {FVIRTKEY, VK_LEFT, back_second},      {FVIRTKEY, VK_RIGHT, forward_second},
        {FVIRTKEY, VK_OEM_COMMA, back_step},   {FVIRTKEY, VK_OEM_PERIOD, forward_step},
        {FVIRTKEY, 'L', toggle_loop},          {FVIRTKEY, 'F', frame_effect},
    };
    return CreateAcceleratorTableW(keys, static_cast<int>(std::size(keys)));
}

bool create_window() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW main_class{};
    main_class.lpfnWndProc = window_proc;
    main_class.hInstance = instance;
    main_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    main_class.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    main_class.lpszClassName = L"ajfx_player";
    RegisterClassW(&main_class);
    WNDCLASSW view_class{};
    view_class.style = CS_DBLCLKS;
    view_class.lpfnWndProc = view_proc;
    view_class.hInstance = instance;
    view_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    view_class.lpszClassName = L"ajfx_player_view";
    RegisterClassW(&view_class);

    g.menu = build_menu();
    g.window = CreateWindowExW(WS_EX_ACCEPTFILES, main_class.lpszClassName, L"ajfx player",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                               nullptr, g.menu, instance, nullptr);
    if (g.window == nullptr) {
        return false;
    }
    g.view = CreateWindowExW(0, view_class.lpszClassName, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, g.window,
                             nullptr, instance, nullptr);
    g.play = CreateWindowExW(0, WC_BUTTONW, L"Pause", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 1, 1, g.window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(play_button)), instance, nullptr);
    g.slider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 0, 0, 1, 1,
                               g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(timeline)), instance, nullptr);
    SendMessageW(g.slider, TBM_SETRANGEMAX, FALSE, timeline_resolution);
    SendMessageW(g.slider, TBM_SETPAGESIZE, 0, timeline_resolution / 10);
    g.clock = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 1, 1, g.window, nullptr,
                              instance, nullptr);
    g.length = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0,
                               1, 1, g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(length_box)), instance,
                               nullptr);
    for (const double seconds : lengths) {
        SendMessageW(g.length, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((std::to_wstring(static_cast<int>(seconds)) + L" s loop").c_str()));
    }
    SendMessageW(g.length, CB_SETCURSEL, 1, 0); // 10 s
    g.status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                               g.window, nullptr, instance, nullptr);
    g.accelerators = build_accelerators();
    apply_font();
    layout();
    return true;
}

// One frame: advance the clock, step to it, draw, present.
void tick() {
    const Clock::time_point now = Clock::now();
    const double elapsed = std::min(std::chrono::duration<double>(now - g.previous).count(), 0.1);
    g.previous = now;
    if (g.player && g.playing && !g.scrubbing) {
        g.time += elapsed;
        if (g.time >= g.duration) {
            if (g.looping) {
                g.time = 0.0;
            } else {
                g.time = g.duration;
                g.playing = false;
            }
        }
        update_controls();
    }

    RECT client{};
    GetClientRect(g.view, &client);
    const auto w = static_cast<std::uint32_t>(client.right);
    const auto h = static_cast<std::uint32_t>(client.bottom);
    if (w == 0U || h == 0U) {
        Sleep(16);
        return;
    }
    if (w != g.width || h != g.height) {
        g.device->destroy_texture(g.target);
        ajfx::Result<ajfx::gpu::Texture> target =
            g.device->create_render_target(w, h, ajfx::gpu::TextureFormat::Rgba8Unorm, "player view");
        if (!target) {
            return;
        }
        g.target = target.value();
        g.width = w;
        g.height = h;
    }

    const Clock::time_point start = Clock::now();
    if (g.player) {
        g.player->seek(current_step());
        g.player->renderer().render(g.player->simulation(), g.orbit.camera(static_cast<float>(w) / static_cast<float>(h)),
                                    g.target, &samples::background);
    } else {
        g.device->begin_rendering(g.target, &samples::background);
        g.device->end_rendering();
    }
    const ajfx::Result<bool> ran = g.device->flush();
    g.frame_ms = 0.9 * g.frame_ms + 0.1 * std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (!ran) {
        g.playing = false;
        show_error(L"The GPU reported an error", ran.error().message);
        g.player.reset();
        update_title();
    }
    (void)g.device->present(g.view, g.target);

    if (now >= g.status_due) {
        g.status_due = now + std::chrono::milliseconds{250};
        update_status();
    }
}

int run(const std::optional<std::wstring>& file) {
    if (!create_window()) {
        return 1;
    }
    ShowWindow(g.window, SW_SHOWDEFAULT);
    update_title();
    update_controls();
    update_status();
    if (file) {
        open(*file);
    }
    MSG message{};
    while (true) {
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return static_cast<int>(message.wParam);
            }
            if (!TranslateAcceleratorW(g.window, g.accelerators, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (IsIconic(g.window)) {
            Sleep(30);
            continue;
        }
        tick();
    }
}

// Renders one frame of `file` offscreen to a PNG: the player's loading and
// framing path without a window, for automated checks.
int screenshot(const std::wstring& file, const std::wstring& png, double seconds) {
    const std::optional<std::vector<std::uint8_t>> bytes = samples::read_file(file);
    if (!bytes) {
        return 3;
    }
    ajfx::Result<ajfx::Effect> effect = ajfx::read_package(*bytes);
    if (!effect) {
        std::fprintf(stderr, "%s\n", effect.error().message.c_str());
        return 3;
    }
    ajfx::Result<std::unique_ptr<samples::Player>> player =
        samples::Player::create(*g.device, g.compiler.get(), std::move(effect).value(), &*bytes);
    if (!player) {
        std::fprintf(stderr, "%s\n", player.error().message.c_str());
        return 3;
    }
    samples::Orbit orbit;
    if (const auto box = player.value()->bounds_until(seconds)) {
        orbit.frame(*box);
    }
    player.value()->seek(static_cast<std::uint64_t>(seconds * player.value()->effect().simulation_rate));
    const ajfx::gpu::Texture target =
        g.device->create_render_target(960U, 540U, ajfx::gpu::TextureFormat::Rgba8Unorm, "screenshot").value();
    player.value()->renderer().render(player.value()->simulation(), orbit.camera(960.0F / 540.0F), target,
                                      &samples::background);
    ajfx::Result<std::vector<std::uint8_t>> pixels = g.device->read_texture(target);
    if (!pixels) {
        return 3;
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, png.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(std::max(length - 1, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, png.c_str(), -1, narrow.data(), length, nullptr, nullptr);
    std::printf("%s\n", player.value()->kernel_note().c_str());
    return viewer::write_png(narrow, 960U, 540U, pixels.value()) ? 0 : 3;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);

    if (!args.empty() && args[0] == L"--register") {
        return register_association() ? 0 : 1;
    }
    if (!args.empty() && args[0] == L"--unregister") {
        return unregister_association() ? 0 : 1;
    }

    ajfx::Result<std::unique_ptr<ajfx::d3d12::Device>> device = ajfx::d3d12::create_device();
    if (!device) {
        device = ajfx::d3d12::create_device({.warp = true});
    }
    if (!device) {
        MessageBoxW(nullptr, widen(device.error().message).c_str(), L"ajfx player needs Direct3D 12",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    g.device = std::move(device).value();
    // Optional: packages with precompiled kernels play without it.
    if (ajfx::Result<std::unique_ptr<ajfx::gpu::ShaderCompiler>> compiler = ajfx::d3d12::create_shader_compiler()) {
        g.compiler = std::move(compiler).value();
    }

    int result = 0;
    const auto shot = std::find(args.begin(), args.end(), L"--screenshot");
    if (shot != args.end() && shot + 1 != args.end() && !args.empty() && args[0] != L"--screenshot") {
        double seconds = 2.0;
        const auto at = std::find(args.begin(), args.end(), L"--seconds");
        if (at != args.end() && at + 1 != args.end()) {
            seconds = _wtof((at + 1)->c_str());
        }
        result = screenshot(args[0], *(shot + 1), seconds);
    } else {
        result = run(args.empty() ? std::nullopt : std::optional<std::wstring>{args[0]});
    }

    g.player.reset();
    if (g.device) {
        g.device->destroy_texture(g.target);
        (void)g.device->flush();
    }
    g.compiler.reset();
    g.device.reset();
    if (g.font != nullptr) {
        DeleteObject(g.font);
    }
    return result;
}
