#define NOMINMAX

#include "thprac_wine_attach.h"
#include "thprac_identify.h"
#include "thprac_log.h"
#include "thprac_log_dir.h"
#include "thprac_gui_locale.h"
#include "thprac_utils.h"
#include "thprac_native.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace THPrac {
namespace WineAttach {

namespace {

// ---------------------------------------------------------------------------
// Wine <-> Unix helpers
// ---------------------------------------------------------------------------

typedef char* (__cdecl* wine_get_unix_file_name_t)(LPCWSTR);
typedef WCHAR* (__cdecl* wine_get_dos_file_name_t)(LPCSTR);

FARPROC WineExport(const char* name) {
    for (const wchar_t* module : { L"kernel32.dll", L"kernelbase.dll", L"ntdll.dll" }) {
        if (HMODULE h = GetModuleHandleW(module)) {
            if (FARPROC p = GetProcAddress(h, name)) {
                return p;
            }
        }
    }
    return nullptr;
}

// Windows path -> Unix path (UTF-8). Empty on failure.
std::string ToUnixPath(const std::wstring& dosPath) {
    if (auto fn = (wine_get_unix_file_name_t)WineExport("wine_get_unix_file_name")) {
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

// Unix path -> Windows path (falls back to the Z: drive).
std::wstring ToDosPath(const std::string& unixPath) {
    if (auto fn = (wine_get_dos_file_name_t)WineExport("wine_get_dos_file_name")) {
        if (WCHAR* p = fn(unixPath.c_str())) {
            std::wstring ret = p;
            HeapFree(GetProcessHeap(), 0, p);
            return ret;
        }
    }
    std::wstring ret = L"Z:" + utf8_to_utf16(unixPath.c_str());
    for (wchar_t& c : ret) {
        if (c == L'/') c = L'\\';
    }
    return ret;
}

bool WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size());
    CloseHandle(h);
    return ok;
}

std::string ReadAll(const std::wstring& path) {
    std::string ret;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return ret;
    }
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) {
        ret.append(buf, got);
        if (ret.size() > (1u << 20)) break;
    }
    CloseHandle(h);
    return ret;
}

// ---------------------------------------------------------------------------
// Diagnostics. Besides the launcher log, details go to $HOME/thprac-wine-attach.log
// (HOME is normally shared with Steam's containers) and %APPDATA%\thprac\wine_attach.log
// so that failures can be diagnosed from outside/inside the Wine prefix.
// ---------------------------------------------------------------------------
bool g_debug = false;

void DebugLog(const std::string& text, bool force = false) {
    if (!g_debug && !force) {
        return;
    }
    log_printf("%s\n", text.c_str());

    // One shared location, reachable from every prefix and the host.
    static std::wstring path = LogPath(L"wine_attach.log");
    std::string line = text + "\n";
    if (!path.empty()) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            SetFilePointer(h, 0, nullptr, FILE_END);
            WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
            CloseHandle(h);
        }
    }
}

bool HasDone(const std::string& out) {
    return out == "DONE\n" || out.find("\nDONE\n") != std::string::npos;
}

struct Candidate {
    int pid = 0;
    std::string name;   // UTF-8 executable name
    std::string prefix; // Unix path of the Wine prefix
    bool otherNs = false;
    THGameID id = ID_UNKNOWN;
};

struct ScriptResult {
    std::string out;
    std::string log;
    bool done = false;
};

bool RunScript(const char* mode, int target, bool needNs, DWORD timeoutMs, ScriptResult& result,
    const std::string& extraVars = std::string()) {
    (void)extraVars;
    std::wstring dir = LogDirectory();
    if (dir.empty()) {
        DebugLog("LogDirectory failed");
        return false;
    }
    std::wstring base = dir + L"thprac_xattach_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount());
    std::wstring outW = base + L".out";
    std::wstring logW = base + L".out.log";

    wchar_t self[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, self, (DWORD)std::size(self))) {
        DebugLog("GetModuleFileNameW failed");
        return false;
    }
    std::wstring folder;
    if (!THPrac::NativeBundle::PrepareFolder(folder)) {
        DebugLog("cannot extract native Unix helper from bundle");
        return false;
    }
    std::wstring helperW = folder + L"\\" + THPrac::NativeBundle::XAttachName;
    if (GetFileAttributesW(helperW.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DebugLog("native Unix helper missing: " + utf16_to_utf8(helperW.c_str()));
        return false;
    }
    if (!WriteAll(outW, "")) {
        DebugLog("cannot create temp file in " + utf16_to_utf8(dir.c_str()));
        return false;
    }
    std::string outU = ToUnixPath(outW);
    std::string logU = ToUnixPath(logW);
    std::string selfU = ToUnixPath(self);
    std::string helperU = ToUnixPath(helperW);
    DebugLog(std::string("[") + mode + "] out=" + outU + " self=" + selfU + " helper=" + helperU);
    if (outU.empty() || logU.empty() || selfU.empty() || helperU.empty()) {
        DebugLog("cannot translate paths to Unix form; cross-prefix attach unavailable");
        DeleteFileW(outW.c_str());
        return false;
    }
    std::wstring helperDos = ToDosPath(helperU);
    std::wstring cmd = L"\"" + helperDos + L"\" " + utf8_to_utf16(mode) + L" \"" + utf8_to_utf16(helperU.c_str())
        + L"\" \"" + utf8_to_utf16(outU.c_str()) + L"\" \"" + utf8_to_utf16(logU.c_str())
        + L"\" \"" + utf8_to_utf16(selfU.c_str()) + L"\" "
        + std::to_wstring(target) + L" " + (needNs ? L"1" : L"0")
        + (extraVars.find("HOST_DISPATCH=1") != std::string::npos ? L" --host" : L"");
    DebugLog("cmd: " + utf16_to_utf8(cmd.c_str()));

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    bool started = CreateProcessW(helperDos.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi);
    if (!started) {
        DebugLog("CreateProcessW(native helper) failed, error " + std::to_string(GetLastError()));
    } else {
        DWORD start = GetTickCount();
        for (;;) {
            if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
            if (HasDone(ReadAll(outW))) break;
            if (GetTickCount() - start > timeoutMs) {
                DebugLog("helper timed out");
                break;
            }
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    result.out = ReadAll(outW);
    result.log = ReadAll(logW);
    result.done = HasDone(result.out);
    if (!result.log.empty()) DebugLog("helper log:\n" + result.log, true);
    DebugLog(std::string("[") + mode + "] done=" + (result.done ? "1" : "0") + " started=" + (started ? "1" : "0")
        + " output bytes=" + std::to_string(result.out.size()), true);
    DeleteFileW(outW.c_str());
    DeleteFileW(logW.c_str());
    return started && result.done;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        lines.emplace_back(text, pos, end - pos);
        pos = end + 1;
    }
    return lines;
}

std::vector<Candidate> ParseCandidates(const std::string& out) {
    std::vector<Candidate> list;
    for (const auto& line : SplitLines(out)) {
        std::string field[4];
        size_t pos = 0;
        int count = 0;
        while (count < 4) {
            size_t tab = line.find('\t', pos);
            if (tab == std::string::npos) {
                field[count++] = line.substr(pos);
                break;
            }
            field[count++] = line.substr(pos, tab - pos);
            pos = tab + 1;
        }
        if (count < 4 || field[0].empty() || field[0].find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        Candidate c;
        c.pid = atoi(field[0].c_str());
        c.name = field[1];
        c.prefix = field[2];
        c.otherNs = field[3] == "1";
        std::wstring wname = utf8_to_utf16(c.name.c_str());
        c.id = ParseExeName(wname.c_str(), wname.size());
        if (c.pid > 0 && !c.name.empty() && !c.prefix.empty()) {
            list.push_back(std::move(c));
        }
    }
    return list;
}

std::set<int> g_tried; // Unix PIDs already attempted (non-manual polling only)
ULONGLONG g_lastScan = 0;
bool g_scannedOnce = false;

} // namespace

bool IsWine() {
    static int cached = -1;
    if (cached < 0) {
        // Newer Wine builds may no longer export wine_get_version, so look at
        // several independent signs: ntdll exports, the Unix root exposed as
        // Z:, and variables Wine/Proton pass through from the host.
        bool wine = false;
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            for (const char* name : { "wine_get_version", "wine_get_build_id", "wine_get_host_version", "wine_get_unix_file_name" }) {
                if (GetProcAddress(ntdll, name)) wine = true;
            }
        }
        if (!wine && WineExport("wine_get_unix_file_name")) wine = true;
        if (!wine && GetFileAttributesW(L"Z:\\proc\\self") != INVALID_FILE_ATTRIBUTES) wine = true;
        if (!wine && GetEnvironmentVariableW(L"WINEPREFIX", nullptr, 0) > 0) wine = true;
        if (!wine && GetEnvironmentVariableW(L"STEAM_COMPAT_DATA_PATH", nullptr, 0) > 0) wine = true;
        cached = wine ? 1 : 0;
        log_printf("thprac: cross-prefix attach: Wine detected = %d\n", cached);
    }
    return cached == 1;
}

std::string DosToUnixPath(const std::string& dosUtf8) {
    if (dosUtf8.empty() || !IsWine()) {
        return {};
    }
    std::wstring wide = utf8_to_utf16(dosUtf8.c_str());
    // wine_get_unix_file_name builds a path even for files that do not exist; for a
    // path registered in another prefix that would be a wrong Linux path.
    if (GetFileAttributesW(wide.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return ToUnixPath(wide);
}

std::string UnixToDosPath(const std::string& unixUtf8) {
    if (unixUtf8.empty() || unixUtf8[0] != '/' || !IsWine()) {
        return {};
    }
    return utf16_to_utf8(ToDosPath(unixUtf8).c_str());
}

// Steam AppID of a game whose container the attach helper can be dispatched
// into directly (see TryInnerAppAttach); nullptr when unknown.
static const char* KnownSteamAppId(THGameID wanted) {
    return wanted == ID_TH06NC ? "4659620" : nullptr;
}

// The plain scan cannot see processes of other Steam containers from inside a
// pressure-vessel sandbox (their /proc entries are unreadable and nsenter to
// the host fails with EPERM). For games with a known AppID the attach helper
// can instead be dispatched straight into the game's own container via
// steam-runtime-launch-client, where the local wineserver sees the game.
// Returns 1 when a game was attached, 0 when the helper ran cleanly but no
// game appeared within its wait window, -1 when the dispatch itself failed.
static int TryInnerAppAttach(THGameID wanted, bool prompt, const char* steamAppId) {
    const char* appId = (steamAppId && *steamAppId) ? steamAppId : KnownSteamAppId(wanted);
    if (!appId) {
        return false;
    }
    char normalizedAppId[24];
    // Normalize a 64-bit shortcut GameID down to its 32-bit AppID.
    {
        unsigned long long v = strtoull(appId, nullptr, 10);
        if (v > 0xFFFFFFFFull) {
            snprintf(normalizedAppId, sizeof(normalizedAppId), "%llu", v >> 32);
            appId = normalizedAppId;
        }
    }
    DebugLog(std::string("no scan candidates; dispatching attach into app container ") + appId, true);
    // The dispatched helper waits for the target Wine process, then up to 60s
    // for the game to become attachable.
    // (THPRAC_ATTACH_WAIT), so one dispatch covers the whole launch window.
    ScriptResult inner;
    if (!RunScript("apply_inner", 0, false, 150000, inner, std::string("INNERAPP=") + appId)) {
        DebugLog("apply_inner: script failed to start/complete", true);
        return -1;
    }
    int status = -1;
    for (const auto& line : SplitLines(inner.out)) {
        if (line == "RC 0") {
            status = 1;
        } else if (line == "RC 1") {
            // The helper ran, waited for the game and timed out without one.
            if (status < 0) status = 0;
        } else if (line.rfind("ERR ", 0) == 0) {
            DebugLog(line.substr(4));
        }
    }
    DebugLog(std::string("apply_inner result: status=") + std::to_string(status), true);
    if (status == 1 && prompt) {
        log_mbox(0, MB_ICONASTERISK | MB_OK, S(THPRAC_PR_COMPLETE), S(THPRAC_PR_INFO_ATTACHED));
    }
    return status;
}

static bool AttachOtherPrefixesSync(THGameID wanted, bool manual, bool prompt, bool* prompted, const char* steamAppId) {
    // FindAndAttach is polled every frame while waiting for a launched game.
    ULONGLONG now = GetTickCount64();
    if (!manual && g_lastScan && now - g_lastScan < 3000) {
        return false;
    }
    g_lastScan = now;
    g_debug = manual || !g_scannedOnce;
    g_scannedOnce = true;
    if (manual) {
        g_tried.clear();
    }
    DebugLog(std::string("AttachOtherPrefixes wanted=") + std::to_string((int)wanted) + " manual=" + (manual ? "1" : "0") + " prompt=" + (prompt ? "1" : "0"), true);

    // Inside a Proton container the launcher cannot read other containers'
    // /proc entries, but the --alongside-steam service can run the helper on
    // the host, where the scan sees all wineservers and the apply can enter
    // the game's container. The inner dispatch is done by the script itself.
    const std::string extraVars = IsProtonContainer() ? "HOST_DISPATCH=1" : "";

    ScriptResult scan;
    if (!RunScript("scan", 0, false, 15000, scan, extraVars)) {
        DebugLog("scan failed to start/complete", true);
        return false;
    }

    // One attempt per prefix, preferring a process with a known game name.
    std::map<std::string, Candidate> chosen;
    for (const auto& c : ParseCandidates(scan.out)) {
        if (g_tried.count(c.pid)) {
            DebugLog("candidate pid " + std::to_string(c.pid) + " already tried");
            continue;
        }
        if (wanted != ID_UNKNOWN) {
            if (c.id != wanted) {
                DebugLog("candidate " + c.name + " ignored: not the requested game");
                continue;
            }
        } else if (c.id == ID_UNKNOWN && !manual) {
            DebugLog("candidate " + c.name + " ignored: unknown name, not a manual request");
            continue; // Unknown names are only considered on explicit request.
        }
        auto it = chosen.find(c.prefix);
        if (it == chosen.end() || (it->second.id == ID_UNKNOWN && c.id != ID_UNKNOWN)) {
            chosen[c.prefix] = c;
        }
    }
    DebugLog("eligible prefixes: " + std::to_string(chosen.size()), true);

    bool handled = false;
    for (auto& [prefix, c] : chosen) {
        g_tried.insert(c.pid);

        if (prompt) {
            if (prompted) *prompted = true;
            std::string title = c.name + " [" + prefix + "]";
            if (log_mboxf(0, MB_YESNO, S(THPRAC_PR_APPLY), S(THPRAC_PR_ASK_ATTACH), title.c_str()) != IDYES) {
                continue;
            }
        }

        DebugLog("applying to " + c.name + " (pid " + std::to_string(c.pid) + ") in Wine prefix " + prefix + (c.otherNs ? " [other namespace]" : ""));
        ScriptResult apply;
        bool ran = RunScript("apply", c.pid, c.otherNs, 120000, apply, extraVars);

        bool success = false;
        for (const auto& line : SplitLines(apply.out)) {
            if (line == "RC 0") success = true;
            else if (line.rfind("ERR ", 0) == 0) DebugLog(line.substr(4));
        }
        handled = true;
        DebugLog(std::string("apply result: ran=") + (ran ? "1" : "0") + " success=" + (success ? "1" : "0"), true);
        if (ran && success) {
            if (prompt) {
                if (prompted) *prompted = true;
                log_mbox(0, MB_ICONASTERISK | MB_OK, S(THPRAC_PR_COMPLETE), S(THPRAC_PR_INFO_ATTACHED));
            }
        } else {
            if (prompted) *prompted = true;
            if (prompt) log_mbox(0, MB_ICONERROR | MB_OK, S(THPRAC_PR_ERROR), S(THPRAC_PR_ERR_ATTACH));
        }
    }
    if (!handled) DebugLog("no cross-prefix game handled", true);
    return handled;
}

static std::atomic<bool> g_asyncRunning = false;
static std::atomic<bool> g_asyncSuccess = false;

struct AsyncScanArgs {
    THGameID wanted;
    std::string steamAppId;
};

static DWORD WINAPI AsyncAttachThread(LPVOID param) {
    auto* args = static_cast<AsyncScanArgs*>(param);
    THGameID wanted = args->wanted;
    std::string steamAppId = args->steamAppId;
    delete args;

    bool dummyPrompted = false;
    bool success = AttachOtherPrefixesSync(wanted, false, false, &dummyPrompted, steamAppId.c_str());
    if (success) {
        g_asyncSuccess.store(true, std::memory_order_release);
    }
    g_asyncRunning.store(false, std::memory_order_release);
    return 0;
}

bool AttachOtherPrefixes(THGameID wanted, bool manual, bool prompt, bool* prompted, const char* steamAppId) {
    if (!IsWine()) {
        DebugLog("skip: not running under Wine", true);
        return false;
    }
    // Set for the helper process we start inside another prefix, so that it
    // only handles its own prefix and never recurses.
    if (GetEnvironmentVariableW(L"THPRAC_WINE_LOCAL_ONLY", nullptr, 0) > 0) {
        DebugLog("skip: THPRAC_WINE_LOCAL_ONLY set", true);
        return false;
    }

    if (!manual) {
        if (g_asyncSuccess.exchange(false, std::memory_order_acq_rel)) {
            return true;
        }
        if (g_asyncRunning.load(std::memory_order_acquire)) {
            return false;
        }
        ULONGLONG now = GetTickCount64();
        if (g_lastScan && now - g_lastScan < 3000) {
            return false;
        }
        g_asyncRunning.store(true, std::memory_order_release);
        auto* args = new AsyncScanArgs{ wanted, steamAppId ? steamAppId : "" };
        HANDLE hThread = CreateThread(nullptr, 0, AsyncAttachThread, args, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            g_asyncRunning.store(false, std::memory_order_release);
            delete args;
        }
        return false;
    }

    return AttachOtherPrefixesSync(wanted, manual, prompt, prompted, steamAppId);
}

// ---------------------------------------------------------------------------
// Ongoing-game watcher.
//
// The launcher only auto-attaches games it started itself
// (state->reflectiveLaunchID). A game launched directly through Steam ("steam
// steam://rungameid/...") never sets that state, so it would stay unhooked:
// the startup search happens once before the game runs, and a plain scan
// cannot see other Steam containers from inside a pressure-vessel sandbox.
// While the launcher is open, this watcher keeps one attach helper resident in
// each configured game's own container; the helper attaches the game as soon
// as it appears. Silent by design: no prompts and no success/failure dialogs.
// One attempt per container runs at a time; entries are never removed, so a
// WatchState pointer stays valid for the lifetime of the process.
// ---------------------------------------------------------------------------
struct WatchState {
    std::atomic<bool> busy{ false };
    std::atomic<ULONGLONG> next{ 0 };
};
struct WatchArgs {
    THGameID wanted;
    std::string appId;
    WatchState* state;
};
static std::atomic<int> g_watchActive{ 0 };
static std::mutex g_watchersMutex;
static std::map<std::string, WatchState*> g_watchers;

static DWORD WINAPI WatchThread(LPVOID param) {
    auto* a = static_cast<WatchArgs*>(param);
    // Blocks up to ~90s inside the game's container (the helper waits there
    // for the game to appear); the launcher UI is not affected.
    bool handled = AttachOtherPrefixesSync(a->wanted, false, false, nullptr, a->appId.c_str());
    int status = handled ? 1 : 0;
    ULONGLONG retry = GetTickCount64()
        + (status == 1 ? 60000  // attached; re-check after a pause
            : status == 0 ? 1000 // clean wait; stay resident
            : 15000);            // dispatch failed; back off before retrying
    a->state->next.store(retry, std::memory_order_release);
    a->state->busy.store(false, std::memory_order_release);
    g_watchActive.fetch_sub(1, std::memory_order_release);
    delete a;
    return 0;
}

bool WatchOngoingGame(THGameID wanted, const std::string& appId) {
    if (appId.empty() || !IsWine()) {
        return false;
    }
    if (GetEnvironmentVariableW(L"THPRAC_WINE_LOCAL_ONLY", nullptr, 0) > 0) {
        return false;
    }
    if (g_watchActive.load(std::memory_order_acquire) >= 4) {
        return false; // keep the number of resident helpers bounded
    }
    WatchState* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_watchersMutex);
        auto& entry = g_watchers[appId];
        if (!entry) entry = new WatchState();
        state = entry;
    }
    ULONGLONG now = GetTickCount64();
    if (now < state->next.load(std::memory_order_acquire)) {
        return false;
    }
    bool expected = false;
    if (!state->busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }

    auto* args = new WatchArgs{ wanted, appId, state };
    HANDLE hThread = CreateThread(nullptr, 0, WatchThread, args, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
        g_watchActive.fetch_add(1, std::memory_order_release);
    } else {
        delete args;
        state->busy.store(false, std::memory_order_release);
        state->next.store(now + 15000, std::memory_order_release);
    }
    return true;
}

bool WatchOngoingNcGame() {
    return WatchOngoingGame(ID_TH06NC, "4659620");
}

}
}
