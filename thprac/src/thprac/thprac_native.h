#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>
#include "thprac_attach_log.h"
#include "thprac_native_bundle.h"
#include "thprac_gui_locale.h"
namespace THPrac {
// True when this process runs inside a Proton/pressure-vessel container (a
// Steam shortcut launch). Such containers cannot reach the host Steam client
// directly: the in-container `steam` bootstrap is a private Steam client that
// fails with "Failed to connect to Steam".
inline bool IsProtonContainer() {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        for (const wchar_t* name : { L"STEAM_COMPAT_DATA_PATH", L"STEAM_COMPAT_CLIENT_INSTALL_PATH",
                                     L"STEAM_COMPAT_TOOL_PATHS", L"PRESSURE_VESSEL_FILENAMES" }) {
            wchar_t probe[4] = {};
            if (GetEnvironmentVariableW(name, probe, 4) > 0) {
                cached = 1;
                break;
            }
        }
    }
    return cached == 1;
}

// Extract a steam:// URL from a command line (up to the next whitespace).
inline std::wstring ExtractSteamUrl(const std::wstring& cmd) {
    size_t at = cmd.find(L"steam://");
    if (at == std::wstring::npos) return std::wstring();
    size_t end = at;
    while (end < cmd.size() && cmd[end] != L' ' && cmd[end] != L'\t') ++end;
    return cmd.substr(at, end - at);
}

// Run a command through /bin/sh via the Z: drive mapping.
inline bool RunShellCommand(const wchar_t* command) {
    std::wstring escaped;
    for (auto c = command; *c; ++c) {
        if (*c == L'"') {
            escaped += L"\\\"";
        } else {
            escaped += *c;
        }
    }
    std::wstring params = L"-c \"" + escaped + L"\"";
    // ShellExecuteW resolves the Unix binary through the Z: drive mapping.
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"Z:\\bin\\sh", params.c_str(), nullptr, SW_HIDE);
    return (INT_PTR)result > 32;
}

// Run a host-side shell command from a Wine/Proton process (ntdll exports
// wine_get_version there, and Z: is mapped to the container root). The
// command is handed to /bin/sh -c, so pipes, redirections and env vars work.
// steam:// URLs are special-cased: inside a Proton container they must be
// dispatched to the host Steam client (Steam itself does this through
// `steam-runtime-launch-client --alongside-steam -- xdg-open ...`).
inline bool RunLaunchCommand(const wchar_t* command) {
    std::wstring cmd = command;
    std::wstring url = ExtractSteamUrl(cmd);
    AttachLog("RunLaunchCommand len=%d container=%d url=%d", (int)cmd.size(), IsProtonContainer() ? 1 : 0,
        url.empty() ? 0 : 1);
    if (!url.empty()) {
        if (IsProtonContainer()) {
            std::wstring quoted = L"'" + url + L"'";
            const wchar_t* attempts[] = {
                L"steam-runtime-launch-client --alongside-steam -- xdg-open ",
                L"xdg-open ",
            };
            for (const wchar_t* prefix : attempts) {
                std::wstring attempt = std::wstring(prefix) + quoted;
                AttachLog("RunLaunchCommand container attempt: %ls", attempt.c_str());
                if (RunShellCommand(attempt.c_str())) {
                    AttachLog("RunLaunchCommand container attempt accepted");
                    return true;
                }
            }
            AttachLog("RunLaunchCommand container attempts rejected, falling back");
        } else if (cmd.find(L"steam://") == 0) {
            // Bare URL on a host-visible Z: drive: let the steam client handle it.
            cmd = L"steam " + url;
        }
    }
    return RunShellCommand(cmd.c_str());
}
// Cross-bitness is handled by a companion built from this same source tree.
inline bool LaunchTH06NC(bool practice, DWORD attachPid = 0, bool lowLatency = false) {
    if (!practice && !attachPid && !lowLatency)
        return RunLaunchCommand(L"steam://rungameid/4659620");
    AttachLog("LaunchTH06NC practice=%d pid=%lu lowLatency=%d", practice ? 1 : 0, (unsigned long)attachPid, lowLatency ? 1 : 0);
    std::wstring helper;
    if (NativeBundle::Present()) {
        if (!NativeBundle::Prepare(helper)) {
            AttachLog("LaunchTH06NC bundle prepare failed");
            MessageBoxW(nullptr, L"无法准备新典练习模块。请检查用户缓存目录的写入权限及安全软件的拦截记录。", L"thprac", MB_ICONERROR);
            return false;
        }
    } else {
        AttachLog("LaunchTH06NC no embedded bundle");
        wchar_t self[32768]{};
        if (!GetModuleFileNameW(nullptr, self, 32768)) return false;
        helper = self;
        helper.resize(helper.find_last_of(L"\\/")+1);
        helper += L"thprac_bridge64.exe";
    }
    AttachLogWide(helper.c_str());
    std::wstring command = L"\"" + helper + L"\" " +
        (attachPid ? L"--attach " + std::to_wstring(attachPid) : L"--steam") +
        L" --language " + std::to_wstring(Gui::LocaleGet());
    if(lowLatency)command += L" --low-latency";
    if(!practice)command += L" --no-practice";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    SetLastError(0);
    BOOL created = CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process);
    AttachLog("LaunchTH06NC createprocess=%d err=%lu", created ? 1 : 0, (unsigned long)GetLastError());
    if (!created) {
        MessageBoxW(nullptr, L"无法启动 thprac_bridge64.exe。请将它和 thprac_th06nc.dll 放在 thprac.exe 同一目录。", L"thprac", MB_ICONERROR);
        return false;
    }
    CloseHandle(process.hThread);
    bool success = true;
    if (attachPid) {
        DWORD exitCode = 1;
        DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
        BOOL gotCode = GetExitCodeProcess(process.hProcess, &exitCode);
        AttachLog("LaunchTH06NC bridge wait=%lu code=%lu got=%d", (unsigned long)wait, (unsigned long)exitCode, gotCode ? 1 : 0);
        success = wait == WAIT_OBJECT_0 && gotCode && exitCode == 0;
    }
    CloseHandle(process.hProcess);
    AttachLog("LaunchTH06NC done success=%d", success ? 1 : 0);
    return success;
}
}
