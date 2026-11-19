#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include "thprac_log_dir.h"
#include <io.h>
#include <sys/stat.h>
#ifndef SHANDLE_PTR
#include <basetsd.h>
typedef LONG_PTR SHANDLE_PTR;
#endif
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace THPrac::NativeBundle {
inline void BundleLog(const char* fmt, ...) {
    char msg[2048];
    va_list va;
    va_start(va, fmt);
    vsnprintf(msg, sizeof(msg), fmt, va);
    va_end(va);
    std::wstring path = THPrac::LogPath(L"wine_attach.log");
    if (path.empty()) return;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    std::string line = std::string("unixcmd: ") + msg + "\r\n";
    DWORD written = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(h);
}
// Resource IDs and relative names also appear in tools/package_nc_single.py.
inline constexpr int BundleId = 100;
inline constexpr const wchar_t* XAttachName = L"thprac-xattach";
inline constexpr const wchar_t* Files[] = {
    L"thprac_bridge64.exe", L"thprac_th06nc.dll",
    L"LICENCE",
    L"licenses\\FreeType-LICENSE.txt", L"licenses\\FreeType-FTL.txt",
    L"licenses\\MinHook.txt", L"licenses\\ImGui.txt", L"THIRD_PARTY_NOTICES.txt",
    XAttachName,
};
struct Resource {
    const unsigned char* data = nullptr;
    DWORD size = 0;
};
inline Resource Read(int id) {
    auto module = GetModuleHandleW(nullptr);
    auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return {};
    return {static_cast<const unsigned char*>(LockResource(LoadResource(module, resource))),
        SizeofResource(module, resource)};
}
inline bool Present() {
    auto id = Read(BundleId);
    if (!id.data || id.size != 64) return false;
    for (DWORD i = 0; i < id.size; ++i)
        if (!((id.data[i] >= '0' && id.data[i] <= '9') || (id.data[i] >= 'a' && id.data[i] <= 'f'))) return false;
    return true;
}
inline bool Matches(const std::wstring& path, Resource resource) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart == resource.size;
    unsigned char buffer[65536];
    for (DWORD offset = 0; ok && offset < resource.size;) {
        DWORD count = (std::min)(DWORD(sizeof(buffer)), resource.size - offset), got = 0;
        ok = ReadFile(file, buffer, count, &got, nullptr) && got == count &&
            std::memcmp(buffer, resource.data + offset, count) == 0;
        offset += count;
    }
    CloseHandle(file);
    return ok;
}
inline bool Directory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    auto attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}
inline std::string DosPathToUnix(const wchar_t* path);
inline void MakeExecutable(const std::wstring& path) {
    std::string unixPath = DosPathToUnix(path.c_str());
    if (unixPath.empty()) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, unixPath.c_str(), (int)unixPath.size(), nullptr, 0);
    std::wstring wide(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, unixPath.c_str(), (int)unixPath.size(), wide.data(), n);
    const wchar_t* chmod = (GetFileAttributesW(L"Z:\\bin\\chmod") != INVALID_FILE_ATTRIBUTES)
        ? L"Z:\\bin\\chmod" : L"Z:\\usr\\bin\\chmod";
    std::wstring cmd = std::wstring(L"\"") + chmod + L"\" +x \"" + wide + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(chmod, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        BundleLog("MakeExecutable failed err=%lu", (unsigned long)GetLastError());
    }
}

inline bool Extract(const std::wstring& folder, const std::wstring& path, Resource resource) {
    if (!resource.data || !resource.size) return false;
    if (Matches(path, resource)) return true;
    // Publish complete files atomically. Existing matching DLLs remain untouched
    // while games use them; simultaneous launchers can reuse the winner's file.
    wchar_t temp[MAX_PATH]{};
    if (!GetTempFileNameW(folder.c_str(), L"tnc", 0, temp)) return false;
    HANDLE file = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok) {
        ok = WriteFile(file, resource.data, resource.size, &written, nullptr) && written == resource.size;
        if (ok) ok = FlushFileBuffers(file) != 0;
        CloseHandle(file);
    }
    if (ok) ok = MoveFileExW(temp, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    DeleteFileW(temp);
    if (ok && Matches(path, resource)) {
        // The static Unix helper must be executable for Wine to start it as a
        // native ELF instead of trying to load it as a PE image.
        MakeExecutable(path);
        return true;
    }
    return false;
}
inline bool PrepareFolder(std::wstring& folder) {
    auto id = Read(BundleId);
    if (!id.data || id.size != 64) return false;
    std::wstring version;
    for (DWORD i = 0; i < id.size; ++i) {
        char c = char(id.data[i]);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        version += wchar_t(c);
    }
    wchar_t local[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, local))) return false;
    folder = local;
    for (const auto& component : {std::wstring(L"thprac"), std::wstring(L"th06nc"), version}) {
        folder += L"\\" + component;
        if (!Directory(folder)) return false;
    }
    if (!Directory(folder + L"\\licenses")) return false;
    for (int i = 0; i < int(sizeof(Files) / sizeof(Files[0])); ++i)
        if (!Extract(folder, folder + L"\\" + Files[i], Read(BundleId + 1 + i))) return false;
    return true;
}
inline bool Prepare(std::wstring& helper) {
    std::wstring folder;
    if (!PrepareFolder(folder)) return false;
    helper = folder + L"\\" + Files[0];
    return true;
}

inline std::string DosPathToUnix(const wchar_t* path) {
    using fn_t = char* (__cdecl*)(LPCWSTR);
    fn_t fn = nullptr;
    for (const wchar_t* mod : { L"kernel32.dll", L"kernelbase.dll", L"ntdll.dll" }) {
        if (HMODULE h = GetModuleHandleW(mod)) {
            fn = (fn_t)GetProcAddress(h, "wine_get_unix_file_name");
            if (fn) break;
        }
    }
    if (fn) {
        if (char* p = fn(path)) {
            std::string result = p;
            HeapFree(GetProcessHeap(), 0, p);
            return result;
        }
    }
    if (path[0] == L'Z' && path[1] == L':') {
        std::string result;
        for (const wchar_t* p = path + 2; *p; ++p) result.push_back(*p == L'\\' ? '/' : char(*p));
        return result;
    }
    return {};
}
inline std::wstring UnixPathToDos(const std::string& unixPath) {
    std::wstring result = L"Z:";
    for (unsigned char c : unixPath) result.push_back(c == '/' ? L'\\' : wchar_t(c));
    return result;
}
inline bool FileExists(const wchar_t* path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
inline bool XAttachPath(std::wstring& path) {
    std::wstring folder;
    if (Present() && PrepareFolder(folder)) {
        std::wstring candidate = folder + L"\\" + XAttachName;
        if (FileExists(candidate.c_str())) { path = candidate; return true; }
    }
    wchar_t self[32768] = {};
    if (!GetModuleFileNameW(nullptr, self, (DWORD)std::size(self))) return false;
    std::wstring s(self), candidate;
    size_t slash = s.find_last_of(L"\\/");
    candidate = (slash == std::wstring::npos ? std::wstring() : s.substr(0, slash)) + L"\\" + XAttachName;
    if (!FileExists(candidate.c_str())) return false;
    path = candidate;
    return true;
}
inline std::string HexEncode(const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), n, nullptr, nullptr);
    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(utf8.size() * 2);
    for (unsigned char c : utf8) { hex.push_back(digits[c >> 4]); hex.push_back(digits[c & 15]); }
    return hex;
}
// Run a Unix command through the embedded static ELF helper (no /bin/sh).
// mode: L"async" (start and return), L"try" (wait up to timeoutMs; a command
// still running after the timeout is left alive), L"wait" (full wait).
inline bool RunUnixCommand(const wchar_t* command, const wchar_t* mode = L"try", DWORD timeoutMs = 5000) {
    std::wstring helper;
    if (!XAttachPath(helper)) {
        BundleLog("RunUnixCommand: helper not found");
        return false;
    }
    std::string hex = HexEncode(command);
    std::wstring hexW;
    for (char c : hex) hexW.push_back(wchar_t((unsigned char)c));
    std::wstring logDir = THPrac::LogDirectory();
    std::wstring logPath = (logDir.empty() ? std::wstring(L"C:\\") : logDir + L"\\") +
        L"thprac_unix_cmd_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount()) + L".log";
    std::wstring helperDos = UnixPathToDos(DosPathToUnix(helper.c_str()));
    std::string logUnix = DosPathToUnix(logPath.c_str());
    std::wstring logWide;
    for (unsigned char c : logUnix) logWide.push_back(wchar_t(c));
    std::wstring cmd = L"\"" + helperDos + L"\" cmd " + hexW + L" " + mode + L" "
        + std::to_wstring(timeoutMs) + L" \"" + logWide + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(helperDos.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        BundleLog("RunUnixCommand create failed mode=%ls err=%lu helper=%ls", mode, (unsigned long)GetLastError(), helperDos.c_str());
        return false;
    }
    DWORD waitCode = WAIT_OBJECT_0;
    DWORD code = 1;
    if (wcscmp(mode, L"async") != 0) {
        waitCode = WaitForSingleObject(pi.hProcess, timeoutMs + 10000);
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        code = 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    HANDLE lf = CreateFileW(logPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (lf != INVALID_HANDLE_VALUE) {
        char buf[4096]; DWORD got = 0;
        while (ReadFile(lf, buf, sizeof(buf), &got, nullptr) && got)
            BundleLog("RunUnixCommand helper: %.*s", (int)got, buf);
        CloseHandle(lf);
        DeleteFileW(logPath.c_str());
    }
    BundleLog("RunUnixCommand mode=%ls wait=%lu code=%lu", mode, (unsigned long)waitCode, (unsigned long)code);
    return code == 0;
}
}
