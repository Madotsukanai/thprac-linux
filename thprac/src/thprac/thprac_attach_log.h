#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "thprac_log.h"
#include "thprac_log_dir.h"

namespace THPrac {

inline bool AttachLogEnabled() {
    static bool enabled = [] {
        wchar_t v[8] = {};
        return GetEnvironmentVariableW(L"THPRAC_TRACE", v, 8) > 0;
    }();
    return enabled;
}

inline void AttachLog(const char* fmt, ...) {
    if (!AttachLogEnabled()) return;
    char msg[1024];
    va_list va;
    va_start(va, fmt);
    vsnprintf(msg, sizeof(msg), fmt, va);
    va_end(va);
    log_printf("thprac: attach: %s\n", msg);
    std::wstring path = LogPath(L"wine_attach.log");
    if (!path.empty()) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            SetFilePointer(h, 0, nullptr, FILE_END);
            DWORD w = 0;
            std::string line = std::string("attach: ") + msg + "\r\n";
            WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
            CloseHandle(h);
        }
    }
}

inline void AttachLogWide(const wchar_t* text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string u(n > 1 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, u.data(), n, nullptr, nullptr);
    AttachLog("%s", u.c_str());
}

}
