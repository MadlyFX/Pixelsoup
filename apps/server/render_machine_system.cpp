#include "render_machine_system.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <shellapi.h>
#include <netfw.h>
#endif

#include <array>
#include <cwchar>
#include <iostream>
#include <system_error>
#include <utility>

namespace mediaserver::app {
namespace {

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    return std::string{reinterpret_cast<const char*>(encoded.data()),
                       encoded.size()};
}

#if defined(_WIN32)
[[nodiscard]] std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    static_cast<void>(::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), wide.data(), length));
    return wide;
}

// COM for one call. A thread that already chose another apartment still has
// COM, so that is usable too; only this scope's own initialization is undone.
class ComScope final {
public:
    ComScope() noexcept
        : result_(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope() {
        if (SUCCEEDED(result_)) {
            ::CoUninitialize();
        }
    }
    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_;
};

template <typename Interface> class ComPtr final {
public:
    ComPtr() noexcept = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() {
        if (pointer_ != nullptr) {
            pointer_->Release();
        }
    }
    [[nodiscard]] Interface** out() noexcept { return &pointer_; }
    [[nodiscard]] Interface* operator->() const noexcept { return pointer_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return pointer_ != nullptr;
    }

private:
    Interface* pointer_{};
};

constexpr const wchar_t* run_key =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* run_value = L"Media Server Render Machine";
#endif

} // namespace

bool leave_unshared_console() noexcept {
#if defined(_WIN32)
    std::array<DWORD, 2U> processes{};
    const DWORD attached = ::GetConsoleProcessList(
        processes.data(), static_cast<DWORD>(processes.size()));
    if (attached == 0U) {
        return true;
    }
    if (attached != 1U) {
        return false;
    }
    return ::FreeConsole() != 0;
#else
    return false;
#endif
}

std::filesystem::path current_executable() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 8; ++attempt) {
        const DWORD written = ::GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0U) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path{buffer};
        }
        buffer.resize(buffer.size() * 2U);
    }
    return {};
#else
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : path;
#endif
}

void show_error_dialog(std::string_view title, std::string_view text) {
#if defined(_WIN32)
    const std::wstring wide_title = widen(title);
    const std::wstring wide_text = widen(text);
    static_cast<void>(::MessageBoxW(nullptr, wide_text.c_str(),
                                    wide_title.c_str(),
                                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND));
#else
    std::cerr << title << ": " << text << '\n';
#endif
}

FirewallStatus query_firewall(const std::vector<FirewallRule>& rules) {
#if defined(_WIN32)
    const ComScope com;
    if (!com.usable()) {
        return FirewallStatus::Unknown;
    }
    ComPtr<INetFwPolicy2> policy;
    if (FAILED(::CoCreateInstance(__uuidof(NetFwPolicy2), nullptr,
                                  CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2),
                                  reinterpret_cast<void**>(policy.out()))) ||
        !policy) {
        return FirewallStatus::Unknown;
    }
    long profiles = 0;
    if (SUCCEEDED(policy->get_CurrentProfileTypes(&profiles)) &&
        profiles != 0) {
        bool enabled_anywhere = false;
        for (const NET_FW_PROFILE_TYPE2 profile :
             {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE,
              NET_FW_PROFILE2_PUBLIC}) {
            if ((profiles & profile) == 0) {
                continue;
            }
            VARIANT_BOOL enabled = VARIANT_TRUE;
            // Unreadable counts as on: claiming the firewall is off when it
            // is not would send the operator looking in the wrong place.
            static_cast<void>(policy->get_FirewallEnabled(profile, &enabled));
            enabled_anywhere = enabled_anywhere || enabled != VARIANT_FALSE;
        }
        if (!enabled_anywhere) {
            return FirewallStatus::Off;
        }
    }
    ComPtr<INetFwRules> list;
    if (FAILED(policy->get_Rules(list.out())) || !list) {
        return FirewallStatus::Unknown;
    }
    for (const FirewallRule& rule : rules) {
        const std::wstring name = widen(rule.name);
        BSTR text = ::SysAllocString(name.c_str());
        if (text == nullptr) {
            return FirewallStatus::Unknown;
        }
        ComPtr<INetFwRule> found;
        const HRESULT item = list->Item(text, found.out());
        ::SysFreeString(text);
        if (FAILED(item) || !found) {
            return FirewallStatus::RulesMissing;
        }
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (FAILED(found->get_Enabled(&enabled)) || enabled == VARIANT_FALSE) {
            return FirewallStatus::RulesMissing;
        }
    }
    return FirewallStatus::RulesPresent;
#else
    static_cast<void>(rules);
    return FirewallStatus::Unknown;
#endif
}

struct ElevatedCommand::Impl final {
#if defined(_WIN32)
    HANDLE process{};
#endif
    std::optional<unsigned long> exit_code;
};

ElevatedCommand::ElevatedCommand(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ElevatedCommand::~ElevatedCommand() {
#if defined(_WIN32)
    if (impl_ && impl_->process != nullptr) {
        ::CloseHandle(impl_->process);
    }
#endif
}

std::unique_ptr<ElevatedCommand>
ElevatedCommand::start(const std::string& parameters, std::string& error) {
#if defined(_WIN32)
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = ::GetSystemDirectoryW(
        system.data(), static_cast<UINT>(system.size()));
    if (length == 0U || length >= system.size()) {
        error = "Could not find the Windows command processor.";
        return nullptr;
    }
    // By full path, so a cmd.exe beside this program is never the one run.
    const std::wstring command =
        std::wstring{system.data(), length} + L"\\cmd.exe";
    const std::wstring arguments = widen(parameters);
    // Shell execution may hand off to shell extensions, which expect COM.
    const ComScope com;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = command.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_HIDE;
    if (::ShellExecuteExW(&info) == FALSE) {
        const DWORD failure = ::GetLastError();
        error = failure == ERROR_CANCELLED
                    ? "Windows Firewall was not changed: the administrator "
                      "prompt was declined."
                    : "Could not ask Windows for administrator rights "
                      "(error " +
                          std::to_string(failure) + ").";
        return nullptr;
    }
    if (info.hProcess == nullptr) {
        error = "Windows started the change but did not say how it went.";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->process = info.hProcess;
    return std::unique_ptr<ElevatedCommand>{new ElevatedCommand{std::move(impl)}};
#else
    static_cast<void>(parameters);
    error = "Windows Firewall exists only on Windows.";
    return nullptr;
#endif
}

std::optional<unsigned long> ElevatedCommand::poll() noexcept {
#if defined(_WIN32)
    if (!impl_->exit_code && impl_->process != nullptr &&
        ::WaitForSingleObject(impl_->process, 0U) == WAIT_OBJECT_0) {
        DWORD code = 1U;
        if (::GetExitCodeProcess(impl_->process, &code) == 0) {
            code = 1U;
        }
        impl_->exit_code = code;
    }
#endif
    return impl_->exit_code;
}

std::string
start_independent_process(const std::filesystem::path& executable,
                          const std::vector<std::string>& arguments,
                          const std::filesystem::path& working_directory,
                          bool no_console_window) {
#if defined(_WIN32)
    // CreateProcess takes the executable separately, so the command line is
    // data for it alone and never passes through a shell.
    const auto quote = [](std::wstring_view value) {
        std::wstring output{L"\""};
        std::size_t slashes{};
        for (const wchar_t character : value) {
            if (character == L'\\') {
                ++slashes;
                continue;
            }
            if (character == L'\"') {
                output.append(slashes * 2U + 1U, L'\\');
                output.push_back(character);
                slashes = 0U;
                continue;
            }
            output.append(slashes, L'\\');
            slashes = 0U;
            output.push_back(character);
        }
        output.append(slashes * 2U, L'\\');
        output.push_back(L'\"');
        return output;
    };
    const std::wstring program = executable.wstring();
    std::wstring command_line = quote(program);
    for (const std::string& argument : arguments) {
        const std::wstring wide = widen(argument);
        if (wide.empty() && !argument.empty()) {
            return "an argument is not valid text";
        }
        command_line.push_back(L' ');
        command_line += quote(wide);
    }
    const std::wstring working = working_directory.wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION created{};
    const DWORD flags = CREATE_NEW_PROCESS_GROUP |
                        (no_console_window ? CREATE_NO_WINDOW
                                           : CREATE_NEW_CONSOLE);
    if (::CreateProcessW(program.c_str(), command_line.data(), nullptr,
                         nullptr, FALSE, flags, nullptr,
                         working.empty() ? nullptr : working.c_str(), &startup,
                         &created) == 0) {
        return std::system_category().message(
            static_cast<int>(::GetLastError()));
    }
    ::CloseHandle(created.hThread);
    ::CloseHandle(created.hProcess);
    return {};
#else
    static_cast<void>(executable);
    static_cast<void>(arguments);
    static_cast<void>(working_directory);
    static_cast<void>(no_console_window);
    return "starting another program is not supported here";
#endif
}

bool start_at_sign_in_supported() noexcept {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

std::string sign_in_command(const std::filesystem::path& executable) {
    return '"' + utf8_path(executable) + "\" --render-machine";
}

bool starts_at_sign_in(const std::filesystem::path& executable) {
#if defined(_WIN32)
    DWORD bytes = 0U;
    if (::RegGetValueW(HKEY_CURRENT_USER, run_key, run_value, RRF_RT_REG_SZ,
                       nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return false;
    }
    std::wstring stored(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(HKEY_CURRENT_USER, run_key, run_value, RRF_RT_REG_SZ,
                       nullptr, stored.data(), &bytes) != ERROR_SUCCESS) {
        return false;
    }
    stored.resize(::wcsnlen(stored.c_str(), stored.size()));
    const std::wstring expected = widen(sign_in_command(executable));
    // An entry for another copy of the program is not this one's: ticking the
    // box again points it here.
    return ::CompareStringOrdinal(stored.c_str(),
                                  static_cast<int>(stored.size()),
                                  expected.c_str(),
                                  static_cast<int>(expected.size()),
                                  TRUE) == CSTR_EQUAL;
#else
    static_cast<void>(executable);
    return false;
#endif
}

bool set_start_at_sign_in(const std::filesystem::path& executable,
                          bool enabled) {
#if defined(_WIN32)
    if (!enabled) {
        const LSTATUS removed =
            ::RegDeleteKeyValueW(HKEY_CURRENT_USER, run_key, run_value);
        return removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND;
    }
    const std::wstring command = widen(sign_in_command(executable));
    if (command.empty()) {
        return false;
    }
    return ::RegSetKeyValueW(
               HKEY_CURRENT_USER, run_key, run_value, REG_SZ, command.c_str(),
               static_cast<DWORD>((command.size() + 1U) * sizeof(wchar_t))) ==
           ERROR_SUCCESS;
#else
    static_cast<void>(executable);
    static_cast<void>(enabled);
    return false;
#endif
}

} // namespace mediaserver::app
