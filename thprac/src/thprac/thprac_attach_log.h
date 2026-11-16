#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "thprac_log.h"

namespace THPrac {

inline void AttachLog(const char* fmt, ...) {
    char msg[1024];
    va_list va;
    va_start(va, fmt);
    vsnprintf(msg, sizeof(msg), fmt, va);
    va_end(va);
    log_printf("thprac: attach: %s\n", msg);
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
        std::wstring dir = std::wstring(appdata) + L"\\thprac";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring path = dir + L"\\wine_attach.log";
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
