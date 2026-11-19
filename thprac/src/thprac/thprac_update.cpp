#include "thprac_update.h"
#include "thprac_native_bundle.h"
#include "thprac_attach_log.h"
#include "thprac_log_dir.h"
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
#include <bcrypt.h>

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
    std::string assetHash; // hex SHA-256 from the Releases asset digest
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

static std::wstring utf8ToHexWide(const std::string& hex) {
    std::wstring wide;
    for (char c : hex) wide.push_back(wchar_t((unsigned char)c));
    return wide;
}

// Compute the SHA-256 of a file as lowercase hex. Used to verify the
// downloaded update against the digest published on the Releases asset.
std::string FileSha256(const std::wstring& path) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::string result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok) {
        std::vector<unsigned char> buffer(65536);
        DWORD got = 0;
        while (true) {
            if (!ReadFile(file, buffer.data(), DWORD(buffer.size()), &got, nullptr)) { ok = false; break; }
            if (!got) break;
            if (BCryptHashData(hash, buffer.data(), got, 0) < 0) { ok = false; break; }
        }
        CloseHandle(file);
    }
    unsigned char digest[32]{};
    if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) ok = false;
    if (ok) {
        static const char hex[] = "0123456789abcdef";
        result.reserve(64);
        for (unsigned char c : digest) { result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

// Run a Unix command via the embedded static ELF helper (no /bin/sh).
bool RunSh(const std::string& commandUtf8) {
    std::wstring wide;
    for (unsigned char c : commandUtf8) wide.push_back(wchar_t(c));
    return THPrac::NativeBundle::RunUnixCommand(wide.c_str(), L"wait", 300000);
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
    if (!RunSh(cmd)) { THPrac::AttachLog("HttpGetCurl failed: %s", cmd.c_str()); return false; }
    THPrac::AttachLog("HttpGetCurl ok: %s", cmd.c_str());
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
        THPrac::AttachLog("HttpDownloadCurl failed: %s", cmd.c_str());
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

bool ParseLatestRelease(const std::string& json, std::string& version, std::string& url, std::string& hash) {
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
    std::string fallbackHash;
    yyjson_arr_foreach(assets, idx, max, item) {
        const char* name = yyjson_get_str(yyjson_obj_get(item, "name"));
        const char* download = yyjson_get_str(yyjson_obj_get(item, "browser_download_url"));
        if (!name || !download) continue;
        const char* digest = yyjson_get_str(yyjson_obj_get(item, "digest"));
        std::string assetHash;
        if (digest && strncmp(digest, "sha256:", 7) == 0) assetHash = digest + 7;
        size_t len = strlen(name);
        if (len >= 4 && _stricmp(name + len - 4, ".exe") == 0) {
            if (!_stricmp(name, "thprac-linux.exe")) {
                url = download;
                hash = assetHash;
                break;
            }
            if (!fallback) { fallback = download; fallbackHash = assetHash; }
        }
    }
    yyjson_doc_free(doc);
    if (url.empty() && fallback) { url = fallback; hash = fallbackHash; }
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
    std::string hash;
    if (HttpGet(API_LATEST, json) && ParseLatestRelease(json, version, url, hash)) {
        state = NewerThanCurrent(version) ? UPDATE_AVAILABLE : UPDATE_CURRENT;
        THPrac::AttachLog("update check: version=%s state=%d url=%s", version.c_str(), state, url.c_str());
    } else {
        THPrac::AttachLog("update check failed: json bytes=%zu", json.size());
    }
    UpdateLock();
    gUpdate.state = state;
    if (state == UPDATE_AVAILABLE) {
        gUpdate.version = version;
        gUpdate.assetUrl = url;
        gUpdate.assetHash = hash;
        THPrac::AttachLog("update check: hash=%s", hash.c_str());
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
    std::string expectedHash;
    {
        UpdateLock();
        expectedHash = gUpdate.assetHash;
        UpdateUnlock();
    }
    bool ok = !url.empty() && HttpDownload(url.c_str(), staged);
    if (ok && !expectedHash.empty()) {
        std::string actualHash = FileSha256(staged);
        THPrac::AttachLog("update hash: expected=%s actual=%s", expectedHash.c_str(), actualHash.c_str());
        if (actualHash.empty() || actualHash.size() != expectedHash.size() ||
            strcasecmp(actualHash.c_str(), expectedHash.c_str()) != 0) {
            THPrac::AttachLog("update hash mismatch");
            log_mbox(0, MB_ICONERROR | MB_OK, S(THPRAC_PR_ERROR), S(THPRAC_UPDATE_HASH_MISMATCH));
            ok = false;
        }
    }
    if (ok) {
        // Hand the swap and relaunch to the native Unix helper: Wine reuses the
        // old image (and its single-instance window) when a running exe is
        // replaced and re-launched in-process. The helper waits for this
        // process to exit before swapping the files and starting the new build.
        std::wstring helper;
        std::string exeUnix = ToUnixPath(exe), stagedUnix = ToUnixPath(staged);
        std::wstring oldPath = exe + L".old";
        std::string oldUnix = ToUnixPath(oldPath);
        if (THPrac::NativeBundle::XAttachPath(helper) && !exeUnix.empty() &&
            !stagedUnix.empty() && !oldUnix.empty()) {
            auto hexOf = [](const std::string& u) {
                std::wstring wide;
                for (unsigned char c : u) wide.push_back(wchar_t(c));
                return THPrac::NativeBundle::HexEncode(wide);
            };
            std::wstring logDir = LogDirectory();
            std::wstring logPath = (logDir.empty() ? std::wstring(L"C:\\") : logDir + L"\\") +
                L"thprac_update_" + std::to_wstring(GetCurrentProcessId()) + L".log";
            std::wstring cmd = L"\"" + helper + L"\" update \"" + logPath + L"\" "
                + utf8ToHexWide(hexOf(exeUnix)) + L" " + utf8ToHexWide(hexOf(stagedUnix))
                + L" " + utf8ToHexWide(hexOf(oldUnix)) + L" " + std::to_wstring(GetCurrentProcessId());
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(helper.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                ExitProcess(0);
            }
            THPrac::AttachLog("update helper start failed err=%lu", (unsigned long)GetLastError());
        } else {
            THPrac::AttachLog("update helper or unix paths unavailable");
        }
        ok = false;
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
