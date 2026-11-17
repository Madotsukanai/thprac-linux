#include "thprac_update.h"
#include "thprac_fork.h"
#include "thprac_gui_locale.h"
#include "thprac_utils.h"
#include "thprac_version.h"
#include <shellapi.h>
#include <string>
#include <vector>
#include <yyjson.h>
#include <string.h>
#include <wininet.h>

namespace THPrac {

void OpenReleasePage(HWND window) {
    if (reinterpret_cast<INT_PTR>(ShellExecuteA(window, "open", THPRAC_FORK_RELEASES,
        nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        std::string message = std::string(S(TH06NC_OPEN_LINK_ERROR)) + "\n" THPRAC_FORK_RELEASES;
        MessageBoxW(window, utf8_to_utf16(message.c_str()).c_str(), L"thprac-linux", MB_ICONERROR);
    }
}

namespace {

// GitHub API for the latest release of this fork.
constexpr const char* API_LATEST = "https://api.github.com/repos/Madotsukanai/thprac-linux/releases/latest";

struct UpdateData {
    int state = UPDATE_IDLE;
    bool threadRunning = false;
    bool csInited = false;
    CRITICAL_SECTION cs;
    std::string version;
    std::string assetUrl;
};
UpdateData gUpdate;

void UpdateLock() {
    if (!gUpdate.csInited) {
        // Only ever initialized on the UI thread.
        InitializeCriticalSection(&gUpdate.cs);
        gUpdate.csInited = true;
    }
    EnterCriticalSection(&gUpdate.cs);
}
void UpdateUnlock() { LeaveCriticalSection(&gUpdate.cs); }
void UpdateSetState(int state) { gUpdate.state = state; }

std::wstring ExePath() {
    wchar_t path[32768] = {};
    GetModuleFileNameW(nullptr, path, 32768);
    return path;
}

bool IsWine() {
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}

std::string ToUnixPath(const std::wstring& dosPath) {
    using wine_get_unix_file_name_t = char* (__cdecl*)(LPCWSTR);
    auto fn = (wine_get_unix_file_name_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_unix_file_name");
    if (fn) {
        if (char* p = fn(dosPath.c_str())) {
            std::string ret = p;
            HeapFree(GetProcessHeap(), 0, p);
            return ret;
        }
    }
    // Fallback: the default Z: drive maps the Unix root.
    if (dosPath.size() > 3 && (dosPath[0] == L'Z' || dosPath[0] == L'z') && dosPath[1] == L':' && dosPath[2] == L'\\') {
        std::string ret = utf16_to_utf8(dosPath.c_str() + 2);
        for (char& c : ret) {
            if (c == '\\') c = '/';
        }
        return ret;
    }
    return {};
}

// Run a command through /bin/sh (Wine starts non-PE executables natively).
bool RunSh(const std::string& commandUtf8) {
    std::wstring escaped;
    for (char c : commandUtf8) {
        wchar_t w = (wchar_t)(unsigned char)c;
        if (w == L'"') escaped += L"\\\"";
        else escaped += w;
    }
    std::wstring shCmd = L"\"Z:\\bin\\sh\" -c \"" + escaped + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    bool started = CreateProcessW(L"Z:\\bin\\sh", shCmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!started) return false;
    // Wait at most 5 minutes for the transfer to finish.
    if (WaitForSingleObject(pi.hProcess, 300000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

bool HttpGetWininet(const char* url, std::string& out) {
    HINTERNET session = InternetOpenA("thprac-linux/" THPRAC_FORK_VERSION,
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!session) return false;
    HINTERNET request = InternetOpenUrlA(session, url, nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!request) {
        InternetCloseHandle(session);
        return false;
    }
    DWORD timeoutMs = 60000;
    InternetSetOptionA(request, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    out.clear();
    char buffer[16384];
    DWORD got = 0;
    while (InternetReadFile(request, buffer, sizeof(buffer), &got) && got) {
        out.append(buffer, got);
        if (out.size() > 64 * 1024 * 1024) break;
    }
    InternetCloseHandle(request);
    InternetCloseHandle(session);
    return !out.empty();
}

bool HttpDownloadWininet(const char* url, const std::wstring& path) {
    HINTERNET session = InternetOpenA("thprac-linux/" THPRAC_FORK_VERSION,
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!session) return false;
    HINTERNET request = InternetOpenUrlA(session, url, nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!request) {
        InternetCloseHandle(session);
        return false;
    }
    DWORD timeoutMs = 60000;
    InternetSetOptionA(request, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        InternetCloseHandle(request);
        InternetCloseHandle(session);
        return false;
    }
    char buffer[65536];
    DWORD got = 0;
    bool ok = true;
    while (InternetReadFile(request, buffer, sizeof(buffer), &got) && got) {
        DWORD wrote = 0;
        if (!WriteFile(file, buffer, got, &wrote, nullptr) || wrote != got) {
            ok = false;
            break;
        }
    }
    CloseHandle(file);
    InternetCloseHandle(request);
    InternetCloseHandle(session);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// Wine/Proton fallback using the host's curl or wget.
bool HttpGetCurl(const char* url, std::string& out) {
    if (!IsWine()) return false;
    wchar_t temp[MAX_PATH + 32] = {};
    DWORD n = GetTempPathW(MAX_PATH, temp);
    if (!n) return false;
    std::wstring file = std::wstring(temp) + L"thprac_update_dl";
    std::string unixPath = ToUnixPath(file);
    if (unixPath.empty()) return false;
    std::string cmd = std::string("curl -sL --fail -m 120 '") + url + "' -o '" + unixPath +
        "' || wget -q -T 120 -O '" + unixPath + "' '" + url + "'";
    if (!RunSh(cmd)) return false;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(f);
        return false;
    }
    out.resize((size_t)size.QuadPart);
    DWORD got = 0;
    bool ok = ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    CloseHandle(f);
    DeleteFileW(file.c_str());
    return ok;
}

bool HttpDownloadCurl(const char* url, const std::wstring& path) {
    if (!IsWine()) return false;
    std::string unixPath = ToUnixPath(path);
    if (unixPath.empty()) return false;
    std::string cmd = std::string("curl -sL --fail -m 600 '") + url + "' -o '" + unixPath +
        "' || wget -q -T 600 -O '" + unixPath + "' '" + url + "'";
    if (!RunSh(cmd)) {
        DeleteFileW(path.c_str());
        return false;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0;
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

bool HttpGet(const char* url, std::string& out) {
    if (HttpGetWininet(url, out)) return true;
    return HttpGetCurl(url, out);
}
bool HttpDownload(const char* url, const std::wstring& path) {
    if (HttpDownloadWininet(url, path)) return true;
    return HttpDownloadCurl(url, path);
}

bool ParseLatestRelease(const std::string& json, std::string& version, std::string& url) {
    yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
    if (!doc) return false;
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }
    const char* tag = yyjson_get_str(yyjson_obj_get(root, "tag_name"));
    if (!tag) {
        yyjson_doc_free(doc);
        return false;
    }
    version = tag;
    yyjson_val* assets = yyjson_obj_get(root, "assets");
    yyjson_val* item;
    size_t idx, max;
    const char* fallback = nullptr;
    yyjson_arr_foreach(assets, idx, max, item) {
        const char* name = yyjson_get_str(yyjson_obj_get(item, "name"));
        const char* download = yyjson_get_str(yyjson_obj_get(item, "browser_download_url"));
        if (!name || !download) continue;
        size_t len = strlen(name);
        if (len >= 4 && _stricmp(name + len - 4, ".exe") == 0) {
            if (!_stricmp(name, "thprac-linux.exe")) {
                url = download;
                break;
            }
            if (!fallback) fallback = download;
        }
    }
    yyjson_doc_free(doc);
    if (url.empty() && fallback) url = fallback;
    return !url.empty();
}

bool NewerThanCurrent(const std::string& tag) {
    ThpracVersion latest = ParseVersion(tag.c_str());
    ThpracVersion current = ParseVersion(THPRAC_FORK_VERSION);
    return latest > current;
}

DWORD WINAPI CheckThread(LPVOID) {
    std::string json, version, url;
    int state = UPDATE_FAILED;
    if (HttpGet(API_LATEST, json) && ParseLatestRelease(json, version, url)) {
        state = NewerThanCurrent(version) ? UPDATE_AVAILABLE : UPDATE_CURRENT;
    }
    UpdateLock();
    gUpdate.state = state;
    if (state == UPDATE_AVAILABLE) {
        gUpdate.version = version;
        gUpdate.assetUrl = url;
    }
    gUpdate.threadRunning = false;
    UpdateUnlock();
    return 0;
}

DWORD WINAPI DownloadThread(LPVOID) {
    std::string url, version;
    UpdateLock();
    url = gUpdate.assetUrl;
    version = gUpdate.version;
    UpdateUnlock();
    std::wstring exe = ExePath();
    std::wstring staged = exe + L".update";
    UpdateSetState(UPDATE_DOWNLOADING);
    bool ok = !url.empty() && HttpDownload(url.c_str(), staged);
    if (ok) {
        // Replace the running executable: renaming a running image is allowed.
        std::wstring oldPath = exe + L".old";
        DeleteFileW(oldPath.c_str());
        if (MoveFileExW(exe.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING) &&
            MoveFileExW(staged.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            // Relaunch and exit so the new build takes over.
            std::wstring cmd = L"\"" + exe + L"\"";
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                ExitProcess(0);
            }
            // Relaunch failed: restore the old build.
            DeleteFileW(exe.c_str());
            MoveFileExW(oldPath.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
            ok = false;
        } else {
            ok = false;
        }
    }
    if (!ok) DeleteFileW(staged.c_str());
    UpdateLock();
    gUpdate.state = ok ? UPDATE_AVAILABLE : UPDATE_INSTALL_FAILED;
    gUpdate.threadRunning = false;
    UpdateUnlock();
    return 0;
}

void StartThread(LPTHREAD_START_ROUTINE routine) {
    UpdateLock();
    if (gUpdate.threadRunning) {
        UpdateUnlock();
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, routine, nullptr, 0, nullptr);
    if (thread) {
        gUpdate.threadRunning = true;
        CloseHandle(thread);
    }
    UpdateUnlock();
}

}

void StartUpdateCheck() {
    UpdateLock();
    int state = gUpdate.state;
    UpdateUnlock();
    if (state == UPDATE_IDLE) StartThread(CheckThread);
}

void RestartUpdateCheck() {
    UpdateLock();
    gUpdate.state = UPDATE_CHECKING;
    gUpdate.threadRunning = false;
    UpdateUnlock();
    StartThread(CheckThread);
}

int UpdateQueryStatus(char* versionBuf, size_t versionBufLen) {
    if (versionBuf && versionBufLen) versionBuf[0] = '\0';
    UpdateLock();
    int state = gUpdate.state;
    if ((state == UPDATE_AVAILABLE || state == UPDATE_INSTALL_FAILED) && versionBuf && versionBufLen) {
        strncpy(versionBuf, gUpdate.version.c_str(), versionBufLen - 1);
        versionBuf[versionBufLen - 1] = '\0';
    }
    UpdateUnlock();
    return state;
}

void UpdateDownloadAndInstall() {
    UpdateLock();
    gUpdate.state = UPDATE_DOWNLOADING;
    UpdateUnlock();
    StartThread(DownloadThread);
}

void UpdateCleanupTempFiles() {
    std::wstring exe = ExePath();
    DeleteFileW((exe + L".old").c_str());
    DeleteFileW((exe + L".update").c_str());
}

}
