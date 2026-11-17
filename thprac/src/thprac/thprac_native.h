#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>
#include "thprac_attach_log.h"
#include "thprac_native_bundle.h"
#include "thprac_gui_locale.h"
namespace THPrac {
// Run a host-side shell command from a Wine/Proton process (ntdll exports
// wine_get_version there, and Z: is mapped to the container root). The
// command is handed to /bin/sh -c, so pipes, redirections and env vars work.
inline bool RunLaunchCommand(const wchar_t* command) {
    std::wstring escaped;
    for (auto c = command; *c; ++c) {
        if (*c == L'"') {
            escaped += L"\\\"";
        } else {
            escaped += *c;
        }
    }
    std::wstring params = L"-c \"" + escaped + L"\"";
    AttachLog("RunLaunchCommand len=%d", (int)escaped.size());
    // ShellExecuteW resolves the Unix binary through the Z: drive mapping.
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"Z:\\bin\\sh", params.c_str(), nullptr, SW_HIDE);
    AttachLog("RunLaunchCommand result=%d", (int)(INT_PTR)result);
    return (INT_PTR)result > 32;
}
// Cross-bitness is handled by a companion built from this same source tree.
inline bool LaunchTH06NC(bool practice, DWORD attachPid = 0, bool lowLatency = false) {
    if (!practice && !attachPid && !lowLatency)
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"steam://rungameid/4659620", nullptr, nullptr, SW_SHOWNORMAL)) > 32;
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
