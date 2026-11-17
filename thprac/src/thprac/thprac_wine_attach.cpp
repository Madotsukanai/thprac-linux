#define NOMINMAX

#include "thprac_wine_attach.h"
#include "thprac_identify.h"
#include "thprac_log.h"
#include "thprac_gui_locale.h"
#include "thprac_utils.h"
#include "thprac_native_bundle.h"
#include "thprac_attach_log.h"

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
// MODE, OUT, SELF, TARGET and NEEDNS are prepended by the launcher.
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

own=${WINEPREFIX:-$HOME/.wine}
own=$(readlink -f "$own" 2>/dev/null || printf %s "$own")
ownmnt=$(readlink /proc/self/ns/mnt 2>/dev/null)

to=
command -v timeout >/dev/null 2>&1 && to="timeout 10"

# Ways of entering a pressure-vessel container with nsenter. The container is
# an unprivileged user namespace, so plain "nsenter -m" fails (EPERM); the user
# namespace must be joined first (-U), and its setgroups is denied, hence
# --preserve-credentials (otherwise: "setgroups failed").
nsopts() {
  case $1 in
    1) echo "-U --preserve-credentials -m -r -w" ;;
    2) echo "--preserve-credentials -m -r -w" ;;
    3) echo "-U -m -r -w" ;;
  esac
}
pick_nsenter() {
  command -v nsenter >/dev/null 2>&1 || return 1
  for n in 1 2 3; do
    v=$(nsopts $n)
    $to nsenter -t "$1" $v true >> "$OUT.log" 2>&1
    r=$?
    if [ "$r" = 0 ]; then printf '%s\n' "$v"; return 0; fi
    dbg "   nsenter [$v] rc=$r"
  done
  return 1
}

scan_procs() {
  root=${1:-/proc}
  dbg "== scan root=$root: own=$own ownmnt=$ownmnt uid=$(id -u 2>/dev/null) pid1=$(cat $root/1/comm 2>/dev/null)"
  dbg "   $(grep -E '^NSpid' $root/self/status 2>/dev/null)"
  dbg "   tools: nsenter=$(command -v nsenter) launch-client=${lclient:-$(command -v steam-runtime-launch-client 2>/dev/null)}"
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
      # Which way into the other container works from here?
      if [ -n "$lclient" ] && [ -n "$appid" ]; then
        $to "$lclient" --inside-app="$appid" -- true >> "$OUT.log" 2>&1
        rc_probe=$?
        dbg "   probe launch-client(inside-app) rc=$rc_probe"
        if [ "$rc_probe" != 0 ]; then
          $to "$lclient" --bus-name="com.steampowered.App$appid" -- true >> "$OUT.log" 2>&1
          dbg "   probe launch-client(bus-name) rc=$?"
        fi
      fi
      if v=$(pick_nsenter "$pid"); then
        dbg "   probe nsenter ok: [$v]"
      else
        dbg "   probe nsenter: no working option set"
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

if [ "$MODE" = apply ]; then
  loader=$(envof "$TARGET" WINELOADER)
  if [ -z "$loader" ] || [ ! -x "$loader" ]; then
    exe=$(readlink "/proc/$TARGET/exe" 2>/dev/null)
    exe=${exe% (deleted)}
    dir=${exe%/*}
    loader=
    for c in "$dir/wine64" "$dir/wine" "$dir/../../../bin/wine64" "$dir/../../../bin/wine"; do
      if [ -x "$c" ]; then loader=$c; break; fi
    done
  fi
  [ -n "$loader" ] || loader=wine
  runself=$SELF
  dbg "== apply: target=$TARGET needns=$NEEDNS loader=$loader"
  rc=1
  if [ "$NEEDNS" = 1 ]; then
    # The target lives in another mount namespace (Steam's pressure-vessel
    # container). Its wineserver socket is only reachable from inside, and
    # starting a second wineserver on the same prefix would be harmful, so
    # enter that container or give up.
    dbg "   diag: $(nsenter --version 2>&1 | head -n 1) userns(self)=$(readlink /proc/self/ns/user) userns(target)=$(readlink /proc/$TARGET/ns/user) mntns(target)=$(readlink /proc/$TARGET/ns/mnt)"
    dbg "   diag: unprivileged_userns_clone=$(cat /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null) apparmor_restrict_unprivileged_userns=$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null) max_user_namespaces=$(cat /proc/sys/user/max_user_namespaces 2>/dev/null)"
    dbg "   diag: target $(grep -E '^(Uid|NoNewPrivs|Seccomp):' /proc/$TARGET/status 2>/dev/null | tr '\n\t' '  ')"
    stage="$HOME/.cache/thprac-wine"
    mkdir -p "$stage" 2>/dev/null
    cmp -s "$SELF" "$stage/thprac.exe" 2>/dev/null || cp -f "$SELF" "$stage/thprac.exe"
    runself=$stage/thprac.exe
    tried=0
    appid=$(envof "$TARGET" SteamAppId)
    [ -n "$appid" ] || appid=$(envof "$TARGET" SteamGameId)
    pfx=$(envof "$TARGET" WINEPREFIX)
    if [ -z "$lclient" ]; then lclient=$(find_launch_client 2>/dev/null || true); fi
    if [ -n "$lclient" ] && [ -n "$appid" ]; then
      tried=1
      dbg "   try launch-client inside-app=$appid path=$lclient"
      "$lclient" --inside-app="$appid" --directory=/ \
        --env="WINEPREFIX=$pfx" --env="THPRAC_WINE_LOCAL_ONLY=1" \
        -- "$loader" "$runself" --attach >> "$OUT.log" 2>&1
      rc=$?
      dbg "   launch-client(inside-app) rc=$rc"
      if [ "$rc" != 0 ]; then
        dbg "   try launch-client bus-name app=$appid path=$lclient"
        "$lclient" --bus-name="com.steampowered.App$appid" --directory=/ \
          --env="WINEPREFIX=$pfx" --env="THPRAC_WINE_LOCAL_ONLY=1" \
          -- "$loader" "$runself" --attach >> "$OUT.log" 2>&1
        rc=$?
        dbg "   launch-client(bus-name) rc=$rc"
      fi
      if [ "$rc" != 0 ] && [ "$lclient" != "steam-runtime-launch-client" ] && command -v steam-runtime-launch-client >/dev/null 2>&1; then
        dbg "   retry with steam-runtime-launch-client from PATH (inside-app)"
        steam-runtime-launch-client --inside-app="$appid" --directory=/ \
          --env="WINEPREFIX=$pfx" --env="THPRAC_WINE_LOCAL_ONLY=1" \
          -- "$loader" "$runself" --attach >> "$OUT.log" 2>&1
        rc=$?
        dbg "   launch-client(PATH,inside-app) rc=$rc"
      fi
    fi
    if [ "$rc" != 0 ] && command -v nsenter >/dev/null 2>&1; then
      tried=1
      dbg "   try nsenter"
      if nsv=$(pick_nsenter "$TARGET"); then
        dbg "   nsenter options: [$nsv]"
        LOADER=$loader; RUNSELF=$runself; export LOADER RUNSELF
        nsenter -t "$TARGET" $nsv xargs -0 sh -c 'unset LD_PRELOAD LD_PRELOAD_64 LD_LIBRARY_PATH STEAM_COMPAT_LAUNCHER_SERVICE; exec env -i "$@" env -u WINESERVERSOCKET THPRAC_WINE_LOCAL_ONLY=1 "$LOADER" "$RUNSELF" --attach' sh \
          < "/proc/$TARGET/environ" >> "$OUT.log" 2>&1
        rc=$?
        dbg "   nsenter rc=$rc"
        if [ "$rc" != 0 ]; then
          # Try with an explicit wineserver socket inside the target container.
          sock=$(ls -1t "/proc/$TARGET/root/tmp/.wine-$UID"/server-*/socket 2>/dev/null | head -n 1)
          if [ -n "$sock" ]; then
            sock_in=${sock#"/proc/$TARGET/root"}
            dbg "   retry nsenter with WINESERVERSOCKET=$sock_in"
            nsenter -t "$TARGET" $nsv xargs -0 sh -c 'unset LD_PRELOAD LD_PRELOAD_64 LD_LIBRARY_PATH STEAM_COMPAT_LAUNCHER_SERVICE; exec env -i "$@" WINESERVERSOCKET='"$sock_in"' THPRAC_WINE_LOCAL_ONLY=1 "$LOADER" "$RUNSELF" --attach' sh \
              < "/proc/$TARGET/environ" >> "$OUT.log" 2>&1
            rc=$?
            dbg "   nsenter(socket) rc=$rc"
          fi
        fi
      else
        dbg "   nsenter: cannot enter the container (no option set worked)"
      fi
    fi
    if [ "$tried" = 0 ]; then
      echo "ERR neither steam-runtime-launch-client nor nsenter is usable" >> "$OUT"
    fi
  else
    LOADER=$loader; RUNSELF=$runself; export LOADER RUNSELF
    xargs -0 sh -c 'unset LD_PRELOAD LD_PRELOAD_64 LD_LIBRARY_PATH STEAM_COMPAT_LAUNCHER_SERVICE; exec env -i "$@" env -u WINESERVERSOCKET THPRAC_WINE_LOCAL_ONLY=1 "$LOADER" "$RUNSELF" --attach' sh \
      < "/proc/$TARGET/environ" >> "$OUT.log" 2>&1
    rc=$?
    dbg "   direct rc=$rc"
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

bool RunScript(const char* mode, int target, bool needNs, DWORD timeoutMs, ScriptResult& result) {
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

    wchar_t self[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, self, (DWORD)std::size(self))) {
        DebugLog("GetModuleFileNameW failed");
        return false;
    }

    if (!WriteAll(outW, "")) {
        DebugLog("cannot create temp file in " + utf16_to_utf8(tmp));
        return false;
    }
    std::string outU = ToUnixPath(outW);
    std::string selfU = ToUnixPath(self);
    DebugLog(std::string("[") + mode + "] out=" + outU + " self=" + selfU);
    if (outU.empty() || selfU.empty()) {
        DebugLog("cannot translate paths to Unix form; cross-prefix attach unavailable");
        DeleteFileW(outW.c_str());
        return false;
    }

    std::string script = "MODE=" + ShellQuote(mode)
        + "\nOUT=" + ShellQuote(outU)
        + "\nSELF=" + ShellQuote(selfU)
        + "\nTARGET=" + std::to_string(target)
        + "\nNEEDNS=" + (needNs ? "1" : "0")
        + "\n" + kScriptBody;
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
// LoadLibraryW on a tiny loader DLL. The loader DLL then starts the regular
// thprac attach helper inside the target's prefix/container, where the
// existing Windows API attach path takes over. This bypasses the fragile
// launch-client/nsenter dance entirely when the caller is allowed to ptrace
// the target (kernel.yama.ptrace_scope / cap_sys_ptrace).
// ---------------------------------------------------------------------------
const char kInjectScript[] = R"SH(
LC_ALL=C; export LC_ALL
dbg() { printf '%s\n' "$*" >> "$OUT.log"; }
stage=${THPRAC_STAGE:-$HOME/.cache/thprac-wine}
mkdir -p "$stage" 2>>"$OUT.log" || stage=/tmp/thprac-wine-$(id -u 2>/dev/null || echo 0)
mkdir -p "$stage" 2>>"$OUT.log"
if ! cp -f "$INJ" "$stage/thprac-injector" 2>>"$OUT.log" || ! chmod +x "$stage/thprac-injector" 2>>"$OUT.log"; then
  echo "ERR stage-injector" >> "$OUT"
else
  cp -f "$LOADER" "$stage/thprac_wineloader.dll" 2>>"$OUT.log" || echo "ERR stage-loader" >> "$OUT"
  to_dos() { printf 'Z:%s' "$(printf %s "$1" | tr '/' '\\')"; }
  loader_dos=$(to_dos "$stage/thprac_wineloader.dll")
  self_dos=$(to_dos "$SELF")
  printf '"%s" --attach %s\n' "$self_dos" "$TARGET" > "$stage/thprac_wineloader.cfg"
  printf 'env:THPRAC_WINE_LOCAL_ONLY=1\n' >> "$stage/thprac_wineloader.cfg"
  dbg "inject: pid=$TARGET loader=$loader_dos"
  "$stage/thprac-injector" --pid "$TARGET" --dll "$loader_dos" >> "$OUT.log" 2>&1
  rc=$?
  dbg "inject rc=$rc"
  echo "RC $rc" >> "$OUT"
fi
echo DONE >> "$OUT"
)SH";

struct InjectResult {
    bool ran = false;
    bool success = false;
    bool ptraceDenied = false;
};

std::vector<std::string> SplitLines(const std::string& text);

// Runs the ptrace injector. Returns false if it could not run at all (no
// bundle, no usable temp dir); check the fields otherwise.
bool TryPtraceInject(const Candidate& c, InjectResult& result) {
    result = {};
    if (!NativeBundle::Present()) {
        DebugLog("ptrace inject: no embedded bundle");
        return false;
    }
    auto injector = NativeBundle::Read(NativeBundle::BundleId + 11);
    auto loader = NativeBundle::Read(NativeBundle::BundleId + 12);
    if (!injector.data || !loader.data) {
        DebugLog("ptrace inject: bundle resources missing");
        return false;
    }
    // Materialise both files (NativeBundle::Prepare does this for the whole
    // bundle; reuse it so the files also exist for the helper we start).
    std::wstring helperPath;
    if (!NativeBundle::Prepare(helperPath)) {
        DebugLog("ptrace inject: bundle extraction failed");
        return false;
    }
    wchar_t dir[32768] = {};
    lstrcpynW(dir, helperPath.c_str(), 32000);
    wchar_t* sep = dir + lstrlenW(dir);
    while (sep > dir && *sep != L'\\' && *sep != L'/') --sep;
    *sep = 0;
    std::wstring injectorPath = std::wstring(dir) + L"\\thprac-injector";
    std::wstring loaderPath = std::wstring(dir) + L"\\thprac_wineloader.dll";

    wchar_t self[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, self, (DWORD)std::size(self))) {
        DebugLog("ptrace inject: GetModuleFileNameW failed");
        return false;
    }

    // Staging/result files, same scheme as RunScript.
    wchar_t tmp[MAX_PATH + 1] = {};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n > MAX_PATH) return false;
    std::wstring base = std::wstring(tmp, n) + L"thprac_pinj_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount());
    std::wstring outW = base + L".out";
    std::wstring logW = base + L".out.log";
    if (!WriteAll(outW, "")) return false;
    std::string outU = ToUnixPath(outW);
    std::string injU = ToUnixPath(injectorPath);
    std::string loaderU = ToUnixPath(loaderPath);
    std::string selfU = ToUnixPath(self);
    if (outU.empty() || injU.empty() || loaderU.empty() || selfU.empty()) {
        DebugLog("ptrace inject: path translation failed");
        DeleteFileW(outW.c_str());
        return false;
    }
    DebugLog("ptrace inject: inj=" + injU + " loader=" + loaderU + " self=" + selfU + " pid=" + std::to_string(c.pid));

    std::string script = std::string("INJ=") + ShellQuote(injU)
        + "\nLOADER=" + ShellQuote(loaderU)
        + "\nSELF=" + ShellQuote(selfU)
        + "\nTARGET=" + std::to_string(c.pid)
        + "\nOUT=" + ShellQuote(outU)
        + "\n" + kInjectScript;
    std::wstring scriptW = base + L".sh";
    if (!WriteAll(scriptW, script)) {
        DeleteFileW(outW.c_str());
        return false;
    }
    std::string scriptU = ToUnixPath(scriptW);
    std::wstring sh = ToDosPath("/bin/sh");
    std::wstring cmd = L"\"" + sh + L"\" \"" + utf8_to_utf16(scriptU.c_str()) + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    bool started = CreateProcessW(sh.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != 0;
    if (!started) {
        DebugLog("ptrace inject: CreateProcessW(/bin/sh) failed, error " + std::to_string(GetLastError()));
    } else {
        DWORD start = GetTickCount();
        for (;;) {
            if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
            if (HasDone(ReadAll(outW))) break;
            if (GetTickCount() - start > 60000) {
                DebugLog("ptrace inject: helper timed out");
                break;
            }
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    std::string out = ReadAll(outW);
    std::string log = ReadAll(logW);
    if (!log.empty()) DebugLog("ptrace inject log:\n" + log);
    result.ran = started;
    for (const auto& line : SplitLines(out)) {
        if (line == "RC 0") result.success = true;
        else if (line == "RC 3") result.ptraceDenied = true;
        else if (line.rfind("ERR ", 0) == 0) DebugLog(line.substr(4));
    }
    DeleteFileW(scriptW.c_str());
    DeleteFileW(outW.c_str());
    DeleteFileW(logW.c_str());
    return started;
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
    if (!RunScript("scan", 0, false, 15000, scan)) {
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
            // Preferred: ptrace-based injection (no second Wine process, no
            // container dance). Falls back to the script below on failure.
            InjectResult injected;
            if (TryPtraceInject(c, injected)) {
                ran = true;
                success = injected.success;
                ptraceDenied = injected.ptraceDenied;
            }
        }
        if (!success) {
            ScriptResult apply;
            ran = RunScript("apply", c.pid, c.otherNs, 120000, apply);
            for (const auto& line : SplitLines(apply.out)) {
                if (line == "RC 0") success = true;
                else if (line.rfind("ERR ", 0) == 0) DebugLog(line.substr(4));
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

}
}
