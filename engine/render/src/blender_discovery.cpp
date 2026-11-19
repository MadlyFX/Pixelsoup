#include <mediaserver/render/blender_discovery.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace mediaserver::render {
namespace fs = std::filesystem;
namespace {
using Clock = std::chrono::steady_clock;
bool hex(const std::string& value, std::size_t size) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool fresh(const fs::directory_entry& entry) {
    const auto age = fs::file_time_type::clock::now() - entry.last_write_time();
    return !entry.is_symlink() && entry.is_regular_file() && entry.file_size() <= 4096
        && age >= -std::chrono::seconds{2} && age < std::chrono::seconds{10};
}
// Records a crashed Blender never removed. Live records are rewritten every
// second, so anything this old belongs to no running process.
bool abandoned(const fs::directory_entry& entry) {
    const auto stem = entry.path().stem().stem().string();
    const auto age = fs::file_time_type::clock::now() - entry.last_write_time();
    return hex(stem, 32) && !entry.is_symlink() && entry.is_regular_file() && age > std::chrono::minutes{10};
}
// Writes `text` beside `target` and renames it into place, so a reader never
// sees a partial record.
void replace_file(const fs::path& target, const std::string& text) {
    const auto temporary = fs::path{target.string() + ".tmp"};
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        output << text;
        output.close();
        if (!output) throw std::runtime_error{"Cannot write " + target.filename().string()};
    }
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    // Blender may be reading the previous file this instant; Windows refuses
    // to replace an open file, so try again briefly before giving up.
    for (int attempt = 0;; ++attempt) {
        std::error_code error;
        fs::rename(temporary, target, error);
        if (!error) return;
        if (attempt == 5) throw std::runtime_error{"Cannot write " + target.filename().string() + ": " + error.message()};
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
}
std::string clean_line(std::string value) {
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    if (value.size() > 512) value.resize(512);
    return value;
}
bool regular(const fs::path& path) {
    std::error_code error;
    return !path.empty() && fs::is_regular_file(path, error);
}
// Blender replaces its record every second. Reading it must never make that
// rename fail, so on Windows the file is opened with delete sharing.
std::string read_record(const fs::path& path) {
#if defined(_WIN32)
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string text(4096, '\0');
    DWORD read{};
    const BOOL ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);
    text.resize(ok ? read : 0U);
    return text;
#else
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
#endif
}
#if defined(_WIN32)
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}
std::wstring quote(std::wstring_view value) {
    std::wstring output{L"\""};
    std::size_t slashes{};
    for (const wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'"') { output.append(slashes * 2U + 1U, L'\\'); output.push_back(character); slashes = 0U; continue; }
        output.append(slashes, L'\\'); slashes = 0U; output.push_back(character);
    }
    output.append(slashes * 2U, L'\\');
    output.push_back(L'"');
    return output;
}
fs::path environment_path(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const DWORD size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (size == 0 || size >= value.size()) return {};
    value.resize(size);
    return fs::path{value};
}
fs::path executable_path() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) return {};
    path.resize(size);
    return fs::path{path};
}
// The shell's own association for .blend, which also finds Steam and custom
// installs. It names blender-launcher.exe; blender.exe sits beside it.
fs::path associated_blender() {
    wchar_t value[2048]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CLASSES_ROOT, L"blendfile\\shell\\open\\command", nullptr, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return {};
    std::wstring command{value};
    std::wstring program;
    if (!command.empty() && command.front() == L'"') {
        const auto end = command.find(L'"', 1);
        if (end == std::wstring::npos) return {};
        program = command.substr(1, end - 1);
    } else program = command.substr(0, command.find(L' '));
    const auto candidate = fs::path{program}.parent_path() / L"blender.exe";
    return regular(candidate) ? candidate : fs::path{};
}
// A record outlives a Blender that quit or crashed by up to its expiry; one
// whose process is gone must not stand in for an open Blender.
bool process_running(unsigned long pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code{};
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}
BOOL CALLBACK raise_window(HWND window, LPARAM parameter) {
    DWORD owner{};
    GetWindowThreadProcessId(window, &owner);
    if (owner != static_cast<DWORD>(parameter) || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    return FALSE;
}
#else
fs::path environment_path(const char* name) {
    const auto* value = std::getenv(name);
    return value ? fs::path{value} : fs::path{};
}
fs::path executable_path() {
    std::error_code error;
    auto path = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::path{} : path;
}
bool process_running(unsigned long pid) {
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}
#endif
fs::path search_path(const fs::path& name) {
#if defined(_WIN32)
    const auto path = environment_path(L"PATH").wstring();
    constexpr wchar_t separator = L';';
#else
    const auto path = environment_path("PATH").string();
    constexpr char separator = ':';
#endif
    std::size_t start{};
    while (start <= path.size()) {
        const auto end = path.find(separator, start);
        const auto entry = path.substr(start, end == path.npos ? path.npos : end - start);
        if (!entry.empty() && regular(fs::path{entry} / name)) return fs::path{entry} / name;
        if (end == path.npos) break;
        start = end + 1;
    }
    return {};
}
}

bool valid_link_source(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash != (value[i] == '-')) return false;
        if (!dash && std::string_view{"0123456789abcdef"}.find(value[i]) == std::string_view::npos) return false;
    }
    return value != "00000000-0000-0000-0000-000000000000";
}

bool parse_blender_instance(const std::string& text, BlenderInstance& item) {
    std::istringstream input{text};
    // Python writes the record in text mode, so Windows lines end in CRLF.
    const auto line = [&](std::string& value) {
        if (!std::getline(input, value)) return false;
        if (!value.empty() && value.back() == '\r') value.pop_back();
        return true;
    };
    // Label, the scene shown, status, ready|resync, the open .blend, launch
    // token, process id, the source of the scene shown, every tagged scene's
    // source, and a scene the artist asked to add to Pixelsoup.
    std::string magic, kind, pid, sources, create;
    if (!line(magic) || magic != "MSBLENDER 2" || !line(item.label) || !line(item.scene) || !line(item.status)
        || !line(kind) || !line(item.file) || !line(item.token) || !line(pid)
        || !line(item.active_source) || !line(sources) || !line(create)) return false;
    item.resync_requested = kind == "resync";
    if (!item.file.empty() && !fs::path{std::u8string(item.file.begin(), item.file.end())}.is_absolute()) item.file.clear();
    if (!hex(item.token, 32)) item.token.clear();
    if (!pid.empty() && pid.size() <= 9 && pid.find_first_not_of("0123456789") == std::string::npos) item.pid = std::stoul(pid);
    if (!valid_link_source(item.active_source)) item.active_source.clear();
    {
        std::istringstream list{sources};
        for (std::string source; std::getline(list, source, ',') && item.scene_sources.size() < 64;)
            if (valid_link_source(source)) item.scene_sources.push_back(source);
    }
    if (const auto tab = create.find('\t'); tab != std::string::npos && valid_link_source(create.substr(0, tab))) {
        item.create_source = create.substr(0, tab);
        item.create_name = create.substr(tab + 1, 256);
    }
    return true;
}

struct BlenderDiscovery::Impl {
    struct Launch { std::string token; fs::path file; std::string source, name; };
    struct Activation { std::string id, source, name; bool adopt{}; };
    fs::path directory;
    std::atomic<std::shared_ptr<const BlenderDiscoveryState>> state;
    std::mutex queue_mutex;
    std::vector<Launch> launches;
    std::vector<unsigned long> raises;
    // The latest scene list per instance, and scenes to show, written by the worker.
    std::map<std::string, std::vector<BlenderSceneLink>> links;
    std::vector<Activation> activations;
    std::vector<BlenderLaunchResult> results;
    Clock::time_point swept{};
#if !defined(_WIN32)
    std::vector<pid_t> children;
#endif
    std::jthread worker;
    explicit Impl(fs::path root) : directory{std::move(root)},
        worker{[this](std::stop_token stop) { run(stop); }} {}
    bool queued() {
        std::lock_guard lock{queue_mutex};
        return !launches.empty() || !raises.empty() || !links.empty() || !activations.empty();
    }
    void start(const Launch& launch) {
        BlenderLaunchResult result{launch.token, {}};
        try {
            const auto executable = find_executable();
            if (executable.empty())
                throw std::runtime_error{"Blender 5.2 or newer was not found. Install it, or set MEDIASERVER_BLENDER to blender.exe."};
            const auto integration = integration_directory();
            const auto script = integration / "mediaserver_open.py";
            if (!regular(script))
                throw std::runtime_error{"The LiveLink add-on files were not found beside pixelsoup; reinstall or run from the build tree."};
            const auto file = fs::absolute(launch.file);
            fs::create_directories(file.parent_path());
            std::vector<fs::path> arguments{executable};
            if (regular(file)) arguments.push_back(file);
            for (const auto& item : {fs::path{"--python"}, script, fs::path{"--"}, fs::path{"--mediaserver-file"}, file,
                                     fs::path{"--mediaserver-addon"}, integration})
                arguments.push_back(item);
            if (valid_link_source(launch.source)) {
                arguments.emplace_back("--mediaserver-scene-source");
                arguments.emplace_back(launch.source);
                arguments.emplace_back("--mediaserver-scene-name");
                arguments.emplace_back(std::u8string(launch.name.begin(), launch.name.end()));
            }
#if defined(_WIN32)
            std::wstring command;
            for (const auto& argument : arguments) {
                if (!command.empty()) command.push_back(L' ');
                command += quote(argument.wstring());
            }
            // An explicit environment block: the token must reach this child
            // only, never the Director's own environment.
            std::wstring environment;
            if (auto* block = GetEnvironmentStringsW()) {
                for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1) {
                    if (_wcsnicmp(entry, L"MEDIASERVER_LIVELINK_TOKEN=", 27) == 0) continue;
                    environment.append(entry); environment.push_back(L'\0');
                }
                FreeEnvironmentStringsW(block);
            }
            environment += L"MEDIASERVER_LIVELINK_TOKEN=" + widen(launch.token);
            environment.push_back(L'\0'); environment.push_back(L'\0');
            STARTUPINFOW startup{}; startup.cb = sizeof(startup);
            PROCESS_INFORMATION created{};
            const auto working = file.parent_path().wstring();
            if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
                    environment.data(), working.c_str(), &startup, &created) == 0)
                throw std::runtime_error{"Blender could not be started (error " + std::to_string(GetLastError()) + ")."};
            CloseHandle(created.hThread);
            CloseHandle(created.hProcess);
#else
            std::vector<std::string> owned;
            for (const auto& argument : arguments) owned.push_back(argument.string());
            std::vector<char*> argv;
            for (auto& argument : owned) argv.push_back(argument.data());
            argv.push_back(nullptr);
            std::vector<std::string> variables{"MEDIASERVER_LIVELINK_TOKEN=" + launch.token};
            for (char** entry = environ; entry && *entry; ++entry)
                if (std::string_view{*entry}.rfind("MEDIASERVER_LIVELINK_TOKEN=", 0) != 0) variables.emplace_back(*entry);
            std::vector<char*> envp;
            for (auto& variable : variables) envp.push_back(variable.data());
            envp.push_back(nullptr);
            pid_t child{};
            if (posix_spawn(&child, owned.front().c_str(), nullptr, nullptr, argv.data(), envp.data()) != 0)
                throw std::runtime_error{"Blender could not be started."};
            children.push_back(child);
#endif
        } catch (const std::exception& error) { result.error = error.what(); }
        results.push_back(std::move(result));
        if (results.size() > 8) results.erase(results.begin());
    }
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::vector<Launch> starting;
            std::vector<unsigned long> raising;
            std::map<std::string, std::vector<BlenderSceneLink>> linking;
            std::vector<Activation> activating;
            {
                std::lock_guard lock{queue_mutex};
                starting.swap(launches);
                raising.swap(raises);
                linking.swap(links);
                activating.swap(activations);
            }
            for (const auto& launch : starting) start(launch);
#if defined(_WIN32)
            for (const auto pid : raising) EnumWindows(raise_window, static_cast<LPARAM>(pid));
#else
            static_cast<void>(raising);
            std::erase_if(children, [](pid_t child) { return waitpid(child, nullptr, WNOHANG) != 0; });
#endif
            auto next = std::make_shared<BlenderDiscoveryState>();
            next->launches = results;
            try {
                if (fs::exists(directory)) {
                    const bool sweep = Clock::now() - swept > std::chrono::minutes{1};
                    if (sweep) swept = Clock::now();
                    std::size_t visited{};
                    for (const auto& entry : fs::directory_iterator{directory}) {
                        if (++visited > 512) break;
                        if (sweep && abandoned(entry)) { std::error_code ignored; fs::remove(entry.path(), ignored); continue; }
                        if (entry.path().extension() != ".instance" || !fresh(entry)) continue;
                        BlenderInstance item;
                        item.id = entry.path().stem().string();
                        if (!hex(item.id, 32)) continue;
                        if (!parse_blender_instance(read_record(entry.path()), item)) continue;
                        if (item.pid != 0 && !process_running(item.pid)) {
                            std::error_code ignored;
                            fs::remove(entry.path(), ignored);
                            continue;
                        }
                        next->instances.push_back(std::move(item));
                        if (next->instances.size() >= 64) break;
                    }
                }
                std::sort(next->instances.begin(), next->instances.end(), [](const auto& a, const auto& b) {
                    return a.label != b.label ? a.label < b.label : a.id < b.id;
                });
                const auto present = [&](const std::string& id) {
                    return std::any_of(next->instances.begin(), next->instances.end(),
                        [&](const auto& item) { return item.id == id; });
                };
                // Scene lists and activations. The add-on consumes each file;
                // a newer one replaces it.
                for (const auto& [id, scenes] : linking) {
                    if (!present(id)) continue;
                    std::ostringstream text;
                    text << "MSLINKS 1\n";
                    for (const auto& scene : scenes)
                        text << scene.source << '\t' << scene.port << '\t' << scene.code << '\t' << clean_line(scene.name) << '\n';
                    replace_file(directory / (id + ".links"), text.str());
                }
                for (const auto& activation : activating) {
                    if (!present(activation.id)) continue;
                    replace_file(directory / (activation.id + ".activate"), "MSACTIVATE 1\n" + activation.source + '\n'
                        + clean_line(activation.name) + '\n' + (activation.adopt ? "adopt" : "show") + '\n');
                }
            } catch (const std::exception& error) { next->error = error.what(); }
            state.store(std::move(next));
            for (int tick = 0; tick < 10 && !stop.stop_requested() && !queued(); ++tick)
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
};
fs::path BlenderDiscovery::default_directory() {
#ifdef _WIN32
    char* base{}; std::size_t length{};
    static_cast<void>(_dupenv_s(&base, &length, "LOCALAPPDATA"));
    const auto path = (base ? fs::path{base} : fs::temp_directory_path()) / "Pixelsoup" / "blender";
    std::free(base);
    return path;
#else
    const auto* base = std::getenv("HOME");
    return (base ? fs::path{base} / ".cache" : fs::temp_directory_path()) / "Pixelsoup" / "blender";
#endif
}
fs::path BlenderDiscovery::find_executable() {
#if defined(_WIN32)
    if (const auto configured = environment_path(L"MEDIASERVER_BLENDER"); regular(configured)) return configured;
    // Prefer the newest qualified release; the add-on requires Blender 5.2.
    fs::path best;
    int best_major{}, best_minor{};
    for (const auto* variable : {L"ProgramFiles", L"ProgramW6432"}) {
        const auto root = environment_path(variable) / L"Blender Foundation";
        std::error_code error;
        for (fs::directory_iterator it{root, error}, end; !error && it != end; it.increment(error)) {
            const auto encoded = it->path().filename().u8string();
            const std::string name(encoded.begin(), encoded.end());
            // "Blender 5.2"
            const auto dot = name.find('.');
            if (!name.starts_with("Blender ") || dot == std::string::npos || dot <= 8 || dot + 1 >= name.size()
                || name.find_first_not_of("0123456789", 8) != dot
                || name.find_first_not_of("0123456789", dot + 1) != std::string::npos || name.size() > 16) continue;
            const int major = std::stoi(name.substr(8, dot - 8)), minor = std::stoi(name.substr(dot + 1));
            if (major < 5 || (major == 5 && minor < 2)) continue;
            const auto candidate = it->path() / L"blender.exe";
            if (regular(candidate) && (major > best_major || (major == best_major && minor > best_minor))) {
                best = candidate; best_major = major; best_minor = minor;
            }
        }
    }
    if (!best.empty()) return best;
    if (auto associated = associated_blender(); !associated.empty()) return associated;
    const auto steam = environment_path(L"ProgramFiles(x86)") / L"Steam/steamapps/common/Blender/blender.exe";
    if (regular(steam)) return steam;
    return search_path(L"blender.exe");
#else
    if (const auto configured = environment_path("MEDIASERVER_BLENDER"); regular(configured)) return configured;
    if (const fs::path bundle{"/Applications/Blender.app/Contents/MacOS/Blender"}; regular(bundle)) return bundle;
    return search_path("blender");
#endif
}
fs::path BlenderDiscovery::integration_directory() {
    for (auto start : {executable_path().parent_path(), fs::current_path()}) {
        for (int depth = 0; depth < 8 && !start.empty(); ++depth) {
            if (regular(start / "blender" / "mediaserver_open.py")) return start / "blender";
            if (regular(start / "integrations" / "blender" / "mediaserver_open.py")) return start / "integrations" / "blender";
            if (start == start.parent_path()) break;
            start = start.parent_path();
        }
    }
    return {};
}
BlenderDiscovery::BlenderDiscovery(fs::path directory) : impl_{std::make_unique<Impl>(std::move(directory))} {}
BlenderDiscovery::~BlenderDiscovery() = default;
std::shared_ptr<const BlenderDiscoveryState> BlenderDiscovery::status() const { return impl_->state.load(); }
void BlenderDiscovery::launch(std::string token, fs::path file, std::string scene_source, std::string scene_name) {
    if (!hex(token, 32) || file.empty()) return;
    if (!valid_link_source(scene_source)) scene_source.clear();
    std::lock_guard lock{impl_->queue_mutex};
    if (impl_->launches.size() < 4)
        impl_->launches.push_back({std::move(token), std::move(file), std::move(scene_source), std::move(scene_name)});
}
void BlenderDiscovery::link_scenes(std::string id, std::vector<BlenderSceneLink> scenes) {
    if (!hex(id, 32)) return;
    std::erase_if(scenes, [](const BlenderSceneLink& scene) {
        return !valid_link_source(scene.source) || scene.port < 1024 || scene.port > 65535 || !hex(scene.code, 64);
    });
    if (scenes.size() > 64) scenes.resize(64);
    std::lock_guard lock{impl_->queue_mutex};
    impl_->links[std::move(id)] = std::move(scenes);
}
void BlenderDiscovery::activate(std::string id, std::string source, std::string name, bool adopt) {
    if (!hex(id, 32) || !valid_link_source(source)) return;
    std::lock_guard lock{impl_->queue_mutex};
    if (impl_->activations.size() < 8) impl_->activations.push_back({std::move(id), std::move(source), std::move(name), adopt});
}
void BlenderDiscovery::focus(unsigned long pid) {
    if (pid == 0) return;
    std::lock_guard lock{impl_->queue_mutex};
    if (impl_->raises.size() < 4) impl_->raises.push_back(pid);
}
}
