#pragma once
#include <windows.h>
#include <string>

namespace THPrac {

// All diagnostics live in ~/.cache/thprac-wine so they can be reached both
// from inside every Wine/Proton prefix and from the Linux host.
inline std::wstring LogDirectory() {
    wchar_t home[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"HOME", home, MAX_PATH);
    std::wstring base;
    if (n && n < MAX_PATH && n > 1) {
        if (home[0] == L'/') {
            // Unix path from the host; reachable through Wine's Z: mapping.
            base = std::wstring(L"Z:") + home;
        } else if (home[0] == L'Z' || home[0] == L'z') {
            // Wine may already translate HOME into a DOS path.
            base = home;
        }
        for (wchar_t& c : base) if (c == L'/') c = L'\\';
    }
    // Last resort: derive the home from our own path when it starts with Z:\home\<user>.
    if (base.empty()) {
        wchar_t self[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, self, MAX_PATH) &&
            (self[0] == L'Z' || self[0] == L'z') && self[1] == L':' &&
            _wcsnicmp(self + 2, L"\\home\\", 6) == 0) {
            const wchar_t* user = self + 8;
            const wchar_t* end = wcschr(user, L'\\');
            base = std::wstring(L"Z:\\home\\") + std::wstring(user, end ? size_t(end - user) : wcslen(user));
        }
    }
    if (base.empty()) {
        wchar_t profile[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) base = profile;
    }
    if (base.empty()) return {};
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    std::wstring path = base + L"\\.cache\\thprac-wine";
    // Create parents one by one; existing directories return ERROR_ALREADY_EXISTS.
    size_t at = path.find(L'\\', 3);
    while (at != std::wstring::npos) {
        CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
        at = path.find(L'\\', at + 1);
    }
    CreateDirectoryW(path.c_str(), nullptr);
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) return path;
    return {};
}

inline std::wstring LogPath(const wchar_t* name) {
    std::wstring dir = LogDirectory();
    if (dir.empty()) return name;
    return dir + L"\\" + name;
}

}
