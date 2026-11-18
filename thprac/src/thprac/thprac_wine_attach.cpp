#define NOMINMAX

#include "thprac_wine_attach.h"
#include "thprac_identify.h"
#include "thprac_log.h"
#include "thprac_gui_locale.h"
#include "thprac_utils.h"
#include "thprac_native.h"
#include "thprac_native_bundle.h"
#include "thprac_attach_log.h"
#include "thprac_th06nc.h"
#include "thprac_load_exe.h"

#include <windows.h>

#include <map>
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

std::string ShellQuote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    r += '\'';
    return r;
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

    // Two locations: $HOME (readable from outside the prefix) and the thprac
    // data directory of the current prefix (always writable).
    static std::vector<std::wstring> paths;
    static bool init = false;
    bool first = !init;
    if (!init) {
        init = true;
        wchar_t home[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableW(L"HOME", home, MAX_PATH);
        if (n && n < MAX_PATH) {
            paths.push_back(ToDosPath(utf16_to_utf8(home) + "/thprac-wine-attach.log"));
        }
        wchar_t appdata[MAX_PATH] = {};
        n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        if (n && n < MAX_PATH) {
            CreateDirectoryW((std::wstring(appdata) + L"\\thprac").c_str(), nullptr);
            paths.push_back(std::wstring(appdata) + L"\\thprac\\wine_attach.log");
        }
        wchar_t temp[MAX_PATH] = {};
        n = GetTempPathW(MAX_PATH, temp);
        if (n && n < MAX_PATH) {
            paths.push_back(std::wstring(temp) + L"thprac-wine-attach.log");
        }
    }
    std::string line = text + "\n";
    for (const auto& path : paths) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        DWORD written = 0;
        SetFilePointer(h, 0, nullptr, FILE_END);
        WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
        CloseHandle(h);
    }
}

// ---------------------------------------------------------------------------
// The Unix side. Runs natively through "/bin/sh" (Wine starts non-PE
// executables natively), so it only relies on POSIX sh plus tr/sed/readlink.
// MODE, OUT, INJ, TARGET, NAME, SIG, MODS and MODN are prepended by the launcher.
// ---------------------------------------------------------------------------
const char kScriptBody[] = R"SH(
LC_ALL=C; export LC_ALL

dbg() { printf '%s\n' "$*" >> "$OUT.log"; }
envof() { tr '\0' '\n' < "/proc/$1/environ" 2>/dev/null | sed -n "s/^$2=//p" | head -n 1; }

# Some PATHs expose the legacy-runtime run.sh stub under this name (it fails with
# "exec: --: invalid option"), so only accept the real pressure-vessel binary.
valid_lc() {
  [ -x "$1" ] || return 1
  case ${1##*/} in steam-runtime-launch-client) ;; *) return 1 ;; esac
  case $(readlink -f "$1" 2>/dev/null) in *.sh|*stub*) return 1 ;; esac
  return 0
}
find_launch_client() {
  p=$(command -v steam-runtime-launch-client 2>/dev/null)
  if [ -n "$p" ] && valid_lc "$p"; then printf '%s\n' "$p"; return 0; fi
  for c in \
    "$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4/pressure-vessel/bin/steam-runtime-launch-client" \
    "$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper/pressure-vessel/bin/steam-runtime-launch-client" \
    "$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_soldier/pressure-vessel/bin/steam-runtime-launch-client" \
    "$HOME/.local/share/Steam/steamrt64/pv-runtime/steam-runtime-steamrt/pressure-vessel/bin/steam-runtime-launch-client" \
    "$HOME/.local/share/Steam/ubuntu12_32/steam-runtime/amd64/usr/bin/steam-runtime-launch-client" \
    "$HOME/.local/share/Steam/ubuntu12_32/steam-runtime/usr/bin/steam-runtime-launch-client" \
    "$HOME/.steam/root/ubuntu12_32/steam-runtime/amd64/usr/bin/steam-runtime-launch-client" \
    "$HOME/.steam/root/ubuntu12_32/steam-runtime/usr/bin/steam-runtime-launch-client" \
    "$HOME/.steam/steam/ubuntu12_32/steam-runtime/amd64/usr/bin/steam-runtime-launch-client" \
    "$HOME/.steam/steam/ubuntu12_32/steam-runtime/usr/bin/steam-runtime-launch-client"
  do
    valid_lc "$c" && { printf '%s\n' "$c"; return 0; }
  done
  for c in "$HOME"/.local/share/Steam/steamapps/common/SteamLinuxRuntime_*/pressure-vessel/bin/steam-runtime-launch-client; do
    valid_lc "$c" && { printf '%s\n' "$c"; return 0; }
  done
  return 1
}
lclient=$(find_launch_client 2>/dev/null || true)
command -v timeout >/dev/null 2>&1 && to="timeout 10"

own=${WINEPREFIX:-$HOME/.wine}
ownmnt=$(readlink /proc/self/ns/mnt 2>/dev/null)

scan_procs() {
  root=${1:-/proc}
  dbg "== scan root=$root: own=$own ownmnt=$ownmnt uid=$(id -u 2>/dev/null) pid1=$(cat $root/1/comm 2>/dev/null)"
  dbg "   $(grep -E '^NSpid' $root/self/status 2>/dev/null)"
  dbg "   tools: launch-client=${lclient:-$(command -v steam-runtime-launch-client 2>/dev/null)}"
  total=0; unreadable=0; winelike=0; found=0; tested=0
  for d in "$root"/[0-9]*; do
    pid=${d##*/}
    total=$((total+1))
    exe=$(readlink "$d/exe" 2>/dev/null) || { unreadable=$((unreadable+1)); continue; }
    exe=${exe% (deleted)}
    case ${exe##*/} in
      wine-preloader|wine64-preloader|wine|wine64|wine-stable|wine-development) ;;
      *) continue ;;
    esac
    winelike=$((winelike+1))
    arg0=$(tr '\0' '\n' < "$d/cmdline" 2>/dev/null | head -n 1)
    comm=$(cat "$d/comm" 2>/dev/null)
    name=
    case $arg0 in
      [Cc]:\\[Ww][Ii][Nn][Dd][Oo][Ww][Ss]\\*) dbg "   skip pid=$pid (system) arg0=[$arg0]"; continue ;;
      ?:\\*) name=${arg0##*\\} ;;
    esac
    if [ -z "$name" ]; then
      # Proton/Wine often has a Unix argv0 (e.g. start.exe path or wine-preloader);
      # find the first non-system DOS .exe argument (the actual game).
      name=$(tr '\0' '\n' < "$d/cmdline" 2>/dev/null | while IFS= read -r a; do
        case $a in
          [Cc]:\\[Ww][Ii][Nn][Dd][Oo][Ww][Ss]\\*) continue ;;
          [A-Za-z]:\\*.[Ee][Xx][Ee]) printf '%s\n' "${a##*\\}"; break ;;
          [A-Za-z]:\\*.[Ee][Xx][Ee]\ *) printf '%s\n' "${a##*\\}"; break ;;
        esac
      done)
    fi
    [ -n "$name" ] || name=$comm
    low=$(printf %s "$name" | tr 'A-Z' 'a-z')
    case $low in
      *.exe) ;;
      *) dbg "   skip pid=$pid (not exe) arg0=[$arg0] comm=[$comm]"; continue ;;
    esac
    case $low in
      services.exe|explorer.exe|winedevice.exe|plugplay.exe|svchost.exe|rpcss.exe|conhost.exe|start.exe|cmd.exe|winedbg.exe) dbg "   skip pid=$pid (system) name=[$name]"; continue ;;
      steam.exe|steamwebhelper.exe|gameoverlayui.exe|crashhandler*.exe|thprac*.exe) dbg "   skip pid=$pid (helper) name=[$name]"; continue ;;
    esac
    pfx=$(envof "$pid" WINEPREFIX)
    if [ -z "$pfx" ]; then
      h=$(envof "$pid" HOME)
      pfx=${h:-$HOME}/.wine
    fi
    pfx=$(readlink -f "$pfx" 2>/dev/null || printf %s "$pfx")
    if [ "$pfx" = "$own" ]; then dbg "   skip pid=$pid name=[$name] (own prefix)"; continue; fi
    ns=0
    [ "$(readlink "$d/ns/mnt" 2>/dev/null)" != "$ownmnt" ] && ns=1
    appid=$(envof "$pid" SteamAppId)
    [ -n "$appid" ] || appid=$(envof "$pid" SteamGameId)
    dbg "   found pid=$pid name=[$name] prefix=$pfx otherns=$ns appid=[$appid] exe=$exe"
    printf '%s\t%s\t%s\t%s\n' "$pid" "$name" "$pfx" "$ns" >> "$OUT"
    found=$((found+1))
    if [ "$ns" = 1 ] && [ "$tested" = 0 ]; then
      tested=1
      # Which way into the other container works from here? Only diagnostics;
      # injection is done with the native ptrace injector either way.
      if [ -n "$lclient" ] && [ -n "$appid" ]; then
        $to "$lclient" --inside-app="$appid" -- true >> "$OUT.log" 2>&1
        dbg "   probe launch-client(inside-app) rc=$?"
      fi
    fi
  done
  dbg "   total=$total unreadable=$unreadable winelike=$winelike found=$found"
  [ "$found" -gt 0 ]
}

if [ "$MODE" = scan ]; then
  if ! scan_procs /proc; then
    # Running inside a Proton/pressure-vessel container often exposes only its own
    # PID namespace. Re-scan from the host init namespaces when possible.
    if command -v nsenter >/dev/null 2>&1; then
      dbg "retry scan via host namespaces (no cross-prefix candidates in current view)"
      MODE=_scan_host OUT=$OUT OWN=$own OWNMNT=$ownmnt $to nsenter -t 1 -m -p /bin/sh "$0" >> "$OUT.log" 2>&1 || dbg "nsenter host scan failed rc=$?"
    fi
  fi
fi
if [ "$MODE" = _scan_host ]; then
  own=$OWN; ownmnt=$OWNMNT
  scan_procs /proc || true
fi

# Injection is done exclusively with the native ptrace injector that the
# launcher stages next to this script. No second Wine process and no
# Windows-side cross-process calls are involved. MODS holds DOS paths of the
# modules in load order; the last one is marked with the 'PRAC' signature
# when SIG=1.
if [ "$MODE" = apply ]; then
  stage="$HOME/.cache/thprac-wine"
  mkdir -p "$stage" 2>>"$OUT.log"
  rc=1
  if [ -n "$INJ" ] && [ -x "$INJ" ] && [ -n "$MODS" ]; then
    dbg "== apply: target=$TARGET name=$NAME mods=$MODS"
    i=0
    for m in $MODS; do
      i=$((i+1))
      extra=
      [ "$i" = "$MODN" ] && [ "$SIG" = 1 ] && extra=--sig
      dbg "   try injector: dll=$m extra=$extra"
      "$INJ" --pid "$TARGET" --dll "$m" $extra --result "$stage/thprac_wineloader.result" >> "$OUT.log" 2>&1
      rc=$?
      dbg "   injector rc=$rc"
      [ "$rc" -eq 0 ] || break
    done
  else
    dbg "== apply: injector or module list missing (INJ=[$INJ] MODS=[$MODS])"
  fi
  echo "RC $rc" >> "$OUT"
fi

echo DONE >> "$OUT"
)SH";

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

// ---------------------------------------------------------------------------
// Staging. The native injector and the modules it must load are copied into
// $HOME/.cache/thprac-wine so they are visible from every Wine/Proton
// container (all of them bind-mount the home directory). Returns the injector
// as a Unix path and the modules as DOS paths in load order; the 'PRAC'
// signature is applied to the last module.
// ---------------------------------------------------------------------------
struct StagedInject {
    std::string injector;             // Unix path, executable
    std::vector<std::string> modules; // DOS paths for --dll
    std::string findName;             // game exe basename for --find
};

bool HomeUnixPath(std::string& out) {
    wchar_t home[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"HOME", home, MAX_PATH);
    if (n && n < MAX_PATH && home[0] == L'/') {
        out = utf16_to_utf8(home);
        return true;
    }
    // No HOME (e.g. started by a bare wine without it)? Fall back to the Z:
    // mapping of the profile directory.
    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
        std::string p = ToUnixPath(profile);
        if (!p.empty()) {
            out = p;
            return true;
        }
    }
    return false;
}

bool StageInjectFiles(bool nc, const char* findName, StagedInject& staged) {
    staged = {};
    std::wstring bundleDir;
    if (!NativeBundle::Present() || !NativeBundle::Prepare(bundleDir)) {
        DebugLog("staging: no embedded bundle");
        return false;
    }
    std::string homeU;
    if (!HomeUnixPath(homeU)) {
        DebugLog("staging: cannot resolve $HOME");
        return false;
    }
    std::string stageU = homeU + "/.cache/thprac-wine";
    std::wstring stageD = ToDosPath(stageU);
    if (stageD.empty() || (GetFileAttributesW(stageD.c_str()) == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW(stageD.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)) {
        DebugLog("staging: cannot create " + stageU);
        return false;
    }
    wchar_t self[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, self, (DWORD)std::size(self))) return false;

    // Module copies live under $HOME; build their DOS form through ToDosPath.
    auto dosIn = [&](const char* name) -> std::string {
        std::wstring d = ToDosPath(stageU + "/" + name);
        return d.empty() ? std::string() : utf16_to_utf8(d.c_str());
    };

    // Always refresh the injector: it changes with every build.
    {
        std::wstring src = bundleDir + L"\\thprac-injector";
        std::wstring dst = stageD + L"\\thprac-injector";
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            DebugLog("staging: cannot copy thprac-injector");
            return false;
        }
    }
    staged.injector = stageU + "/thprac-injector";

    if (nc) {
        // freetype is statically linked into the module, so a single load is
        // enough.
        for (const char* name : { "thprac_th06nc.dll" }) {
            std::wstring src = bundleDir + L"\\" + utf8_to_utf16(name);
            std::wstring dst = stageD + L"\\" + utf8_to_utf16(name);
            if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
                DebugLog(std::string("staging: cannot copy ") + name);
                return false;
            }
            staged.modules.push_back(dosIn(name));
        }
        // Hand the launch options over through the filesystem; $HOME is
        // shared between containers, so the module reads them wherever it
        // was loaded.
        LaunchOptions opts{};
        opts.practice = 1;
        opts.language = Gui::LocaleGet();
        WriteLaunchOptionsFile(opts);
        staged.findName = findName && *findName ? findName : "th06nc.exe";
    } else {
        std::string selfU = ToUnixPath(self);
        std::wstring srcD = ToDosPath(selfU);
        if (srcD.empty()) {
            DebugLog("staging: cannot translate the launcher path");
            return false;
        }
        std::wstring dst = stageD + L"\\thprac.exe";
        if (!CopyFileW(srcD.c_str(), dst.c_str(), FALSE)) {
            DebugLog("staging: cannot copy the launcher module");
            return false;
        }
        staged.modules.push_back(dosIn("thprac.exe"));
        staged.findName = findName && *findName ? findName : "thprac.exe";
    }
    for (const auto& m : staged.modules) {
        if (m.empty()) {
            DebugLog("staging: module path translation failed");
            return false;
        }
    }
    return !staged.modules.empty();
}

bool RunScript(const char* mode, int target, const StagedInject& staged, DWORD timeoutMs, ScriptResult& result) {
    wchar_t tmp[MAX_PATH + 1] = {};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n > MAX_PATH) {
        DebugLog("GetTempPathW failed");
        return false;
    }
    // Fresh prefixes (e.g. Proton) may not have the Temp directory yet.
    std::wstring dir(tmp, n);
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t appdata[MAX_PATH] = {};
        DWORD m = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        if (m && m < MAX_PATH) {
            dir = std::wstring(appdata) + L"\\thprac\\";
        }
    }
    std::wstring base = dir + L"thprac_xattach_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount());
    std::wstring scriptW = base + L".sh";
    std::wstring outW = base + L".out";
    std::wstring logW = base + L".out.log";

    if (!WriteAll(outW, "")) {
        DebugLog("cannot create temp file in " + utf16_to_utf8(tmp));
        return false;
    }
    std::string outU = ToUnixPath(outW);
    DebugLog(std::string("[") + mode + "] out=" + outU);
    if (outU.empty()) {
        DebugLog("cannot translate paths to Unix form; cross-prefix attach unavailable");
        DeleteFileW(outW.c_str());
        return false;
    }
    std::string mods;
    std::string script = "MODE=" + ShellQuote(mode)
        + "\nOUT=" + ShellQuote(outU)
        + "\nINJ=" + ShellQuote(staged.injector)
        + "\nTARGET=" + std::to_string(target)
        + "\nNAME=" + ShellQuote(staged.findName)
        + "\nSIG=1"
        + "\nMODN=" + std::to_string(staged.modules.size())
        + "\nMODS=";
    for (size_t i = 0; i < staged.modules.size(); ++i) {
        if (i) mods += " ";
        mods += ShellQuote(staged.modules[i]);
    }
    script += mods + "\n" + kScriptBody;
    if (!WriteAll(scriptW, script)) {
        DebugLog("cannot write helper script");
        DeleteFileW(outW.c_str());
        return false;
    }
    std::string scriptU = ToUnixPath(scriptW);

    std::wstring sh = ToDosPath("/bin/sh");
    std::wstring cmd = L"\"" + sh + L"\" \"" + utf8_to_utf16(scriptU.c_str()) + L"\"";
    DebugLog("cmd: " + utf16_to_utf8(cmd.c_str()));

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    bool started = !scriptU.empty() && CreateProcessW(sh.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (!started) {
        DebugLog("CreateProcessW(/bin/sh) failed, error " + std::to_string(GetLastError()));
    } else {
        DWORD start = GetTickCount();
        for (;;) {
            if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
                break;
            }
            // The script's last line is DONE, in case waiting on a native
            // process is unreliable on this Wine build.
            if (HasDone(ReadAll(outW))) {
                break;
            }
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
    if (!result.log.empty()) {
        DebugLog("helper log:\n" + result.log);
    }
    DebugLog(std::string("[") + mode + "] done=" + (result.done ? "1" : "0") + " started=" + (started ? "1" : "0")
        + " output bytes=" + std::to_string(result.out.size()));
    DeleteFileW(scriptW.c_str());
    DeleteFileW(outW.c_str());
    DeleteFileW(logW.c_str());
    return started && result.done;
}

// ---------------------------------------------------------------------------
// ptrace-based injection. A native helper (tools/thprac_injector.c, shipped in
// the bundle) attaches to the target with ptrace and remotely calls
// ntdll!LdrLoadDll on a tiny loader DLL. The loader DLL then runs the second
// stage inside the target's prefix/container: the cfg command spawns the same
// native injector (now with cap_sys_ptrace set) against the game process to
// LdrLoadDll the full thprac module and mark it applied. This bypasses the
// fragile launch-client/nsenter dance entirely when the caller is allowed to
// ptrace the target (kernel.yama.ptrace_scope / cap_sys_ptrace).
// ---------------------------------------------------------------------------


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
bool g_ptraceDenied = false; // last attach round was blocked by ptrace permissions

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

bool AttachOtherPrefixes(THGameID wanted, bool manual, bool prompt, bool* prompted) {
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

    ScriptResult scan;
    if (!RunScript("scan", 0, StagedInject{}, 15000, scan)) {
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
        bool success = false;
        bool ran = false;
        bool ptraceDenied = false;
        {
            // Injection is done exclusively by the native ptrace injector,
            // staged into the shared home directory. No second Wine process,
            // no Windows-side cross-process calls.
            StagedInject staged;
            if (StageInjectFiles(c.id == ID_TH06NC, c.name.c_str(), staged)) {
                ScriptResult apply;
                ran = RunScript("apply", c.pid, staged, 120000, apply);
                for (const auto& line : SplitLines(apply.out)) {
                    if (line == "RC 0") success = true;
                    else if (line == "RC 3") ptraceDenied = true;
                    else if (line.rfind("ERR ", 0) == 0) DebugLog(line.substr(4));
                }
            }
        }
        g_ptraceDenied |= ptraceDenied;
        handled = true;
        DebugLog(std::string("apply result: ran=") + (ran ? "1" : "0") + " success=" + (success ? "1" : "0"), true);
        if (ran && success) {
            if (prompt) {
                if (prompted) *prompted = true;
                log_mbox(0, MB_ICONASTERISK | MB_OK, S(THPRAC_PR_COMPLETE), S(THPRAC_PR_INFO_ATTACHED));
            }
        } else {
            if (prompted) *prompted = true;
            std::string text = S(THPRAC_PR_ERR_ATTACH);
            if (g_ptraceDenied && S(THPRAC_WINE_PTRACE_CAP)) {
                text += "\n\n";
                text += S(THPRAC_WINE_PTRACE_CAP);
            }
            log_mbox(0, MB_ICONERROR | MB_OK, S(THPRAC_PR_ERROR), text.c_str());
        }
    }
    if (!handled) DebugLog("no cross-prefix game handled", true);
    return handled;
}

// Dispatch the attach helper into a Steam app's own container. Used when a
// containerized launcher (a Steam shortcut) starts a game via a steam:// URL:
// the launcher cannot scan other containers (procfs is masked and nsenter is
// not permitted), so instead of waiting for a scan that never succeeds, the
// helper is started where the game lives through steam-runtime-launch-client
// --inside-app, which is the same channel the apply stage uses.
// Dispatch the native injector into a Steam app's own container. Used when a
// containerized launcher (a Steam shortcut) starts a game via a steam:// URL:
// the launcher cannot scan other containers (procfs is masked), so instead of
// waiting for a scan that never succeeds, the injector is started where the
// game lives through steam-runtime-launch-client --inside-app. Everything it
// needs is staged into $HOME/.cache/thprac-wine beforehand.
void DispatchSteamHook(const wchar_t* steamUrl, THGameID gameId) {
    const wchar_t* rid = wcsstr(steamUrl ? steamUrl : L"", L"rungameid/");
    if (!rid) return;
    unsigned long long id = wcstoull(rid + 10, nullptr, 10);
    if (!id) return;
    // Shortcut GameIDs carry the appid in the high 32 bits; plain appids are
    // used directly for real Steam apps.
    unsigned long long appid = (id >> 32) ? (id >> 32) : id;

    bool nc = gameId == ID_TH06NC;
    std::string exeName = PrimaryExeNameForGame(gameId);
    if (exeName.empty()) exeName = nc ? "th06nc.exe" : "thprac.exe";

    StagedInject staged;
    if (!StageInjectFiles(nc, exeName.c_str(), staged)) {
        DebugLog("hook dispatch: staging failed", true);
        return;
    }

    std::string mods;
    for (size_t i = 0; i < staged.modules.size(); ++i) {
        if (i) mods += " ";
        mods += "'" + staged.modules[i] + "'";
    }
    char appidText[32];
    snprintf(appidText, sizeof(appidText), "%llu", appid);
    std::string injU = staged.injector;
    std::string findName = staged.findName;
    std::string homeU;
    std::string resD;
    if (HomeUnixPath(homeU)) {
        std::wstring resDos = ToDosPath(homeU + "/.cache/thprac-wine/hook.result");
        if (!resDos.empty()) resD = utf16_to_utf8(resDos.c_str());
    }
    if (resD.empty()) resD = "/home-nonexistent/thprac-hook.result";
    std::string snippet = std::string(
        "log=\"$HOME/thprac-hook.log\"\n"
        "echo \"== hook dispatch appid=") + appidText + " $(date 2>/dev/null)\" >> \"$log\"\n"
        "stage=\"$HOME/.cache/thprac-wine\"\n"
        "mkdir -p \"$stage\" 2>>\"$log\"\n"
        "inj=" + ShellQuote(injU) + "\n"
        "name=" + ShellQuote(findName) + "\n"
        "mods=" + mods + "\n"
        "chmod +x \"$inj\" 2>>\"$log\"\n"
        "setcap cap_sys_ptrace=ep \"$inj\" 2>>\"$log\"\n"
        "pfx=\n"
        "for base in ${STEAM_COMPAT_CLIENT_INSTALL_PATH:+\"$STEAM_COMPAT_CLIENT_INSTALL_PATH\"} \"$HOME/.local/share/Steam\" $(sed -n 's/^[[:space:]]*\"path\"[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p' \"$HOME/.local/share/Steam/steamapps/libraryfolders.vdf\" 2>/dev/null); do\n"
        "  [ -n \"$base\" ] || continue\n"
        "  if [ -d \"$base/steamapps/compatdata/$appid/pfx\" ]; then pfx=\"$base/steamapps/compatdata/$appid/pfx\"; break; fi\n"
        "done\n"
        "if [ -z \"$pfx\" ]; then echo \"ERR no compatdata/$appid\" >> \"$log\"; exit 2; fi\n"
        "lclient=$(command -v steam-runtime-launch-client 2>/dev/null)\n"
        "if [ -z \"$lclient\" ]; then echo \"ERR no steam-runtime-launch-client\" >> \"$log\"; exit 3; fi\n"
        "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do\n"
        "  echo \"-- hook attempt $i\" >> \"$log\"\n"
        "  nmods=0\n"
        "  for m in $mods; do nmods=$((nmods+1)); done\n"
        "  ok=1\n"
        "  n=0\n"
        "  for m in $mods; do\n"
        "    n=$((n+1))\n"
        "    extra=\n"
        "    [ \"$n\" = \"$nmods\" ] && extra=--sig\n"
        "    \"$lclient\" --inside-app=\"$appid\" --directory=/ -- \"$inj\" --find \"$name\" --dll \"$m\" $extra --wait 10 --result " + ShellQuote(resD) + " >> \"$log\" 2>&1 || ok=0\n"
        "  done\n"
        "  if [ \"$ok\" = 1 ]; then echo \"OK attached (attempt $i)\" >> \"$log\"; exit 0; fi\n"
        "  sleep 3\n"
        "done\n"
        "echo \"ERR game never appeared\" >> \"$log\"\n"
        "exit 4\n";
    DebugLog("hook dispatch: appid=" + std::string(appidText) + " nc=" + (nc ? "1" : "0") + " via launch-client --inside-app", true);
    RunShellCommand(utf8_to_utf16(snippet.c_str()).c_str());
}
}
}
