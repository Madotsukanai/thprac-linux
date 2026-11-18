// thprac_wineloader.dll: a tiny loader injected into a running Wine process by
// thprac-injector (ptrace). Its only job is to start the thprac attach helper
// inside the same Wine prefix/container as the game, so the helper shares the
// game's wineserver and can use the regular Windows API attach path.
//
// The command line to run is read from "thprac_wineloader.cfg" next to this
// DLL (first line). The outcome is written to "thprac_wineloader.result":
//   ok
//   err <GetLastError>
#include <windows.h>

#ifndef THPRAC_WINLOADER
#error "build with -DTHPRAC_WINLOADER"
#endif

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace {

void Trim(wchar_t* s) {
    wchar_t* e = s + lstrlenW(s);
    while (e > s && (e[-1] == L'\r' || e[-1] == L'\n' || e[-1] == L' ' || e[-1] == L'\t')) *--e = 0;
    while (*s == L' ' || *s == L'\t') ++s;
}

void WriteResult(const wchar_t* dir, const wchar_t* text) {
    wchar_t path[32768];
    lstrcpynW(path, dir, 32000);
    lstrcatW(path, L"\\thprac_wineloader.result");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, text, lstrlenW(text) * sizeof(wchar_t), &wrote, nullptr);
    CloseHandle(f);
}

DWORD WINAPI SpawnThread(void*) {
    wchar_t self[32768] = {};
    GetModuleFileNameW((HMODULE)&__ImageBase, self, 32768);
    wchar_t* sep = self + lstrlenW(self);
    while (sep > self && *sep != L'\\' && *sep != L'/') --sep;
    if (sep == self) return 1;
    *sep = 0; // self now holds the directory
    wchar_t* base = sep + 1;

    // This DLL runs inside the game process, so its pid is the target pid.
    // The cfg command may contain {pid} placeholders, which lets it drive a
    // native (non-Wine) helper such as thprac-injector --pid {pid}.
    wchar_t pidText[16];
    wsprintfW(pidText, L"%lu", GetCurrentProcessId());

    // Remove any stale result file.
    wchar_t result[32768];
    lstrcpynW(result, self, 32000);
    lstrcatW(result, L"\\thprac_wineloader.result");
    DeleteFileW(result);

    wchar_t cfg[32768];
    lstrcpynW(cfg, self, 32000);
    lstrcatW(cfg, L"\\");
    lstrcatW(cfg, base); // thprac_wineloader.dll
    // cfg path = directory + thprac_wineloader.cfg
    wchar_t* dot = cfg + lstrlenW(cfg);
    while (dot > cfg && *dot != L'.') --dot;
    if (dot > cfg) *dot = 0;
    lstrcatW(cfg, L".cfg");

    HANDLE f = CreateFileW(cfg, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        WriteResult(self, L"err config-not-found");
        return 2;
    }
    char buf[4096] = {};
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    if (!got) {
        WriteResult(self, L"err config-empty");
        return 2;
    }
    wchar_t cmd[2048] = {};
    int n = MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, cmd, 2047);
    // Lines after the command may set environment variables: "env:NAME=VALUE".
    wchar_t* envStart = nullptr;
    for (int i = 0; i < n; ++i) {
        if (cmd[i] == L'\n' || cmd[i] == L'\r') {
            cmd[i] = 0;
            if (i + 1 < n) envStart = cmd + i + 1;
            break;
        }
    }
    Trim(cmd);
    if (!cmd[0]) {
        WriteResult(self, L"err config-empty");
        return 2;
    }
    // Substitute {pid} (the host process = the game).
    {
        wchar_t* out = cmd;
        for (wchar_t* p = cmd; *p;) {
            if (p[0] == L'{' && p[1] == L'p' && p[2] == L'i' && p[3] == L'd' && p[4] == L'}') {
                for (wchar_t* d = pidText; *d;) *out++ = *d++;
                p += 5;
            } else {
                *out++ = *p++;
            }
        }
        *out = 0;
    }
    if (envStart) {
        // envStart points into cmd's buffer; process each line in place.
        wchar_t* p = envStart;
        while (p && *p) {
            wchar_t* line = p;
            wchar_t* next = nullptr;
            for (wchar_t* q = p; *q; ++q) {
                if (*q == L'\r') *q = 0;
                if (*q == L'\n') {
                    *q = 0;
                    next = q + 1;
                    break;
                }
            }
            if (wcsncmp(line, L"env:", 4) == 0) {
                wchar_t* eq = wcschr(line + 4, L'=');
                if (eq) {
                    *eq = 0;
                    SetEnvironmentVariableW(line + 4, eq + 1);
                }
            }
            p = (next && *next) ? next : nullptr;
        }
    }

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    wchar_t cmdMut[2048];
    lstrcpynW(cmdMut, cmd, 2048);
    if (!CreateProcessW(nullptr, cmdMut, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        wchar_t msg[64];
        wsprintfW(msg, L"err %lu", GetLastError());
        WriteResult(self, msg);
        return 3;
    }
    CloseHandle(pi.hThread);
    // Wait a moment so the helper is up before the loader DLL is unloaded
    // (the injector leaves the module mapped; no unload happens, but a short
    // wait also lets the helper report early failures).
    WaitForSingleObject(pi.hProcess, 3000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    // A native helper (--result) may already have written the final status
    // (as UTF-8); never clobber a definitive result with our optimistic one.
    {
        unsigned char rbytes[16] = {};
        HANDLE rf = CreateFileW(result, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (rf != INVALID_HANDLE_VALUE) {
            DWORD rgot = 0;
            ReadFile(rf, rbytes, sizeof(rbytes), &rgot, nullptr);
            CloseHandle(rf);
            // The injector writes UTF-8 ("ok..."/"err ..."); our own writes
            // are UTF-16LE (first byte 0x6F followed by 0x00), which will not
            // match the ASCII check below.
            if (rgot >= 2 && rbytes[0] == 'o' && rbytes[1] == 'k') return 0;
            if (rgot >= 3 && rbytes[0] == 'e' && rbytes[1] == 'r' && rbytes[2] == 'r') return 0;
        }
    }
    WriteResult(self, code == STILL_ACTIVE ? L"ok" : (code == 0 ? L"ok" : L"started"));
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE t = CreateThread(nullptr, 0, SpawnThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
