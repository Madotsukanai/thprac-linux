// Native Unix helper for cross-prefix Wine attaching. Built as a static ELF
// and embedded in the launcher; spawned by thprac_wine_attach.cpp instead of
// a generated shell script. Modes:
//   scan  <selfExe> <outFile> <logFile>
//   apply <selfExe> <outFile> <logFile> <launcherExe> <pid> <needNs>
//   run   <envFile> <logFile> <loader> <launcherExe> [args...]
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

extern char** environ;
static const char* kOutPath = nullptr;
static const char* kLogPath = nullptr;
static void redirectLog(const char* path) {
    if (!path || !*path) return;
    kLogPath = path;
    FILE* f = fopen(path, "w");
    if (!f) return;
    dup2(fileno(f), 2);
    fclose(f);
}

static void dbg(const std::string& text) {
    fprintf(stderr, "%s\n", text.c_str());
    fflush(stderr);
}
static void emit(const std::string& text) {
    if (!kOutPath) return;
    FILE* f = fopen(kOutPath, "a");
    if (!f) return;
    fputs(text.c_str(), f);
    fflush(f);
    fclose(f);
}
static std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}
static std::string baseName(const std::string& p) {
    size_t n = p.find_last_of("/\\");
    return n == std::string::npos ? p : p.substr(n + 1);
}
static bool readAll(const std::string& path, std::string& out) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    char buf[8192];
    ssize_t got;
    while ((got = read(fd, buf, sizeof(buf))) > 0) out.append(buf, size_t(got));
    close(fd);
    return got == 0;
}
static std::string readLink(const std::string& path) {
    char buf[4096];
    ssize_t n = readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) return {};
    buf[n] = 0;
    std::string v(buf);
    if (v.size() > 11 && v.compare(v.size() - 11, 11, " (deleted)") == 0) v.resize(v.size() - 11);
    return v;
}
static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> r;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(delim, start);
        if (end == std::string::npos) { r.push_back(s.substr(start)); break; }
        r.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return r;
}
static std::string envValue(const std::string& environText, const std::string& key) {
    std::string prefix = key + "=";
    for (const auto& line : split(environText, '\0')) {
        if (line.size() > prefix.size() && line.compare(0, prefix.size(), prefix) == 0)
            return line.substr(prefix.size());
    }
    return {};
}
static std::string envValue(int pid, const std::string& key) {
    std::string text;
    if (!readAll("/proc/" + std::to_string(pid) + "/environ", text)) return {};
    return envValue(text, key);
}
static bool executable(const std::string& path) {
    return access(path.c_str(), X_OK) == 0;
}
static std::string findLaunchClient() {
    if (const char* path = getenv("PATH")) {
        for (const auto& dir : split(path, ':')) {
            std::string p = (dir.empty() ? std::string(".") : dir) + "/steam-runtime-launch-client";
            if (executable(p)) return p;
        }
    }
    std::string home = getenv("HOME") ? getenv("HOME") : "";
    std::vector<std::string> roots = {
        home + "/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4/pressure-vessel/bin",
        home + "/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper/pressure-vessel/bin",
        home + "/.local/share/Steam/steamapps/common/SteamLinuxRuntime_soldier/pressure-vessel/bin",
        home + "/.local/share/Steam/steamrt64/pv-runtime/steam-runtime-steamrt/pressure-vessel/bin",
        home + "/.local/share/Steam/ubuntu12_32/steam-runtime/amd64/usr/bin",
        home + "/.steam/root/ubuntu12_32/steam-runtime/amd64/usr/bin",
    };
    if (DIR* d = opendir((home + "/.local/share/Steam/steamapps/common").c_str())) {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name.rfind("SteamLinuxRuntime_", 0) == 0)
                roots.push_back(home + "/.local/share/Steam/steamapps/common/" + name + "/pressure-vessel/bin");
        }
        closedir(d);
    }
    for (const auto& dir : roots) {
        std::string p = dir + "/steam-runtime-launch-client";
        if (executable(p)) return p;
    }
    return {};
}
static bool isWineExe(const std::string& path) {
    std::string n = lower(baseName(path));
    return n == "wine-preloader" || n == "wine64-preloader" || n == "wine" || n == "wine64" ||
           n == "wine-stable" || n == "wine-development";
}
static bool systemName(const std::string& low) {
    static const char* names[] = {
        "services.exe", "explorer.exe", "winedevice.exe", "plugplay.exe", "svchost.exe",
        "rpcss.exe", "conhost.exe", "start.exe", "cmd.exe", "winedbg.exe", "tabtip.exe",
        "steam.exe", "steamwebhelper.exe", "gameoverlayui.exe", "crashhandler.exe",
        "xalia.exe",
    };
    for (auto n : names) if (low == n) return true;
    if (low.rfind("crashhandler", 0) == 0 && low.size() > 12 && low.substr(low.size() - 4) == ".exe") return true;
    if (low.rfind("thprac", 0) == 0) return true;
    return false;
}
static std::string exeNameFromCmdline(int pid) {
    std::string text;
    if (!readAll("/proc/" + std::to_string(pid) + "/cmdline", text)) return {};
    for (const auto& arg : split(text, '\0')) {
        if (arg.size() < 5) continue;
        if (!(arg[0] >= 'A' && arg[0] <= 'Z') && !(arg[0] >= 'a' && arg[0] <= 'z')) continue;
        if (arg[1] != ':' || (arg[2] != '\\' && arg[2] != '/')) continue;
        std::string n = lower(baseName(arg));
        if (n == "services.exe" || n == "explorer.exe" || n == "start.exe" || n == "cmd.exe") continue;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".exe") return n;
    }
    return {};
}
static std::string canonicalPrefix(std::string p) {
    if (p.empty()) {
        p = getenv("HOME") ? getenv("HOME") : "";
        p += "/.wine";
    }
    char resolved[4096] = {};
    if (realpath(p.c_str(), resolved)) return resolved;
    return p;
}

static void doScan() {
    std::string ownPfx = canonicalPrefix(getenv("WINEPREFIX") ? getenv("WINEPREFIX") : "");
    std::string ownMnt = readLink("/proc/self/ns/mnt");
    int total = 0, unreadable = 0, winelike = 0, found = 0;
    DIR* d = opendir("/proc");
    if (!d) { dbg("cannot open /proc"); emit("DONE\n"); return; }
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        ++total;
        int pid = atoi(e->d_name);
        std::string exe = readLink("/proc/" + std::to_string(pid) + "/exe");
        if (exe.empty()) { ++unreadable; continue; }
        if (!isWineExe(exe)) continue;
        ++winelike;
        std::string name = exeNameFromCmdline(pid);
        if (name.empty()) {
            std::string comm;
            if (readAll("/proc/" + std::to_string(pid) + "/comm", comm)) name = lower(split(comm, '\n')[0]);
        }
        if (systemName(name)) continue;
        std::string pfx = canonicalPrefix(envValue(pid, "WINEPREFIX"));
        if (pfx == ownPfx) continue;
        bool otherNs = !ownMnt.empty() && readLink("/proc/" + std::to_string(pid) + "/ns/mnt") != ownMnt;
        std::string appid = envValue(pid, "SteamAppId");
        if (appid.empty()) appid = envValue(pid, "SteamGameId");
        dbg("found pid=" + std::to_string(pid) + " name=[" + name + "] prefix=" + pfx +
            " otherns=" + (otherNs ? "1" : "0") + " appid=[" + appid + "] exe=" + exe);
        emit(std::to_string(pid) + "\t" + name + "\t" + pfx + "\t" + (otherNs ? "1" : "0") + "\t" + appid + "\n");
        ++found;
    }
    closedir(d);
    dbg("total=" + std::to_string(total) + " unreadable=" + std::to_string(unreadable) +
        " winelike=" + std::to_string(winelike) + " found=" + std::to_string(found));
    emit("DONE\n");
}

static int waitChild(pid_t child) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

struct ExecResult { bool started = false; int code = -1; };

static ExecResult runChild(const std::vector<std::string>& args,
                           const std::vector<std::string>& env = {}, bool clearEnv = false) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (const auto& e : env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    pid_t child = fork();
    if (child < 0) return {};
    if (child == 0) {
        if (clearEnv) environ = envp.data();
        else for (auto e : env) putenv(const_cast<char*>(e.c_str()));
        execv(args[0].c_str(), argv.data());
        dbg("execv failed: " + args[0] + " errno=" + std::to_string(errno));
        _exit(127);
    }
    return {true, waitChild(child)};
}

static std::vector<std::string> targetEnv(int pid, const std::string& launcherExe) {
    std::string text;
    readAll("/proc/" + std::to_string(pid) + "/environ", text);
    std::vector<std::string> result;
    for (const auto& line : split(text, '\0')) {
        if (line.empty()) continue;
        std::string key = line.substr(0, line.find('='));
        if (key == "LD_PRELOAD" || key == "LD_PRELOAD_64" || key == "LD_LIBRARY_PATH" ||
            key == "STEAM_COMPAT_LAUNCHER_SERVICE" || key == "WINESERVERSOCKET") continue;
        result.push_back(line);
    }
    result.push_back("THPRAC_WINE_LOCAL_ONLY=1");
    result.push_back("THPRAC_ATTACH_WAIT=60");
    (void)launcherExe;
    return result;
}

static bool nsenterWorks(int pid) {
    auto r = runChild({"/usr/bin/nsenter", "-t", std::to_string(pid), "-U", "--preserve-credentials", "-m", "-r", "-w", "true"}, {}, true);
    return r.started && r.code == 0;
}

static void doApply(const std::string& selfExe, const std::string& launcherExe, int pid, bool needNs) {
    std::string environText;
    readAll("/proc/" + std::to_string(pid) + "/environ", environText);
    std::string loader = envValue(environText, "WINELOADER");
    auto fromHost = [](std::string p) {
        // Inside pressure-vessel the host filesystem appears under /run/host.
        if (p.rfind("/run/host/", 0) == 0) p = p.substr(9);
        return p;
    };
    if (!loader.empty()) loader = fromHost(loader);
    if (loader.empty() || !executable(loader)) {
        std::string exe = fromHost(readLink("/proc/" + std::to_string(pid) + "/exe"));
        std::string dir = exe.substr(0, exe.find_last_of('/'));
        for (const auto& rel : {"/wine64", "/wine", "/../../../bin/wine64", "/../../../bin/wine",
                                "/../../bin/wine64", "/../../bin/wine"}) {
            std::string p = dir + rel;
            if (executable(p)) { loader = p; break; }
        }
    }
    if (loader.empty() || !executable(loader)) {
        // Derive the loader from this helper's own parent Wine process (the
        // launcher), which uses a compatible Wine build.
        std::string selfExe = readLink("/proc/" + std::to_string(getppid()) + "/exe");
        selfExe = fromHost(selfExe);
        std::string dir = selfExe.substr(0, selfExe.find_last_of('/'));
        for (const auto& rel : {"/wine64", "/wine", "/../../../bin/wine64", "/../../../bin/wine",
                                "/../../bin/wine64", "/../../bin/wine"}) {
            std::string p = dir + rel;
            if (executable(p)) { loader = p; break; }
        }
    }
    dbg("loader resolved: " + loader);
    std::string pfx = envValue(environText, "WINEPREFIX");
    std::string appid = envValue(environText, "SteamAppId");
    if (appid.empty()) appid = envValue(environText, "SteamGameId");
    if (!appid.empty()) {
        unsigned long long v = strtoull(appid.c_str(), nullptr, 10);
        if (v > 0xFFFFFFFFull) appid = std::to_string(v >> 32);
    }
    dbg("apply pid=" + std::to_string(pid) + " needNs=" + std::to_string(needNs) +
        " loader=" + loader + " appid=" + appid);

    std::vector<std::string> env = targetEnv(pid, launcherExe);
    std::string lclient = findLaunchClient();
    int rc = -1;
    if (needNs) {
        if (!lclient.empty() && !appid.empty()) {
            for (int variant = 0; variant < 2 && rc != 0; ++variant) {
                std::vector<std::string> args = {lclient};
                if (variant == 0) args.push_back("--inside-app=" + appid);
                else args.push_back("--bus-name=com.steampowered.App" + appid);
                args.push_back("--directory=/");
                args.push_back("--env=WINEPREFIX=" + pfx);
                args.push_back("--env=THPRAC_WINE_LOCAL_ONLY=1");
                args.push_back("--env=THPRAC_ATTACH_WAIT=60");
                args.push_back("--");
                args.push_back(loader);
                args.push_back(launcherExe);
                args.push_back("--attach");
                dbg("launch-client variant=" + std::to_string(variant) + " path=" + lclient);
                auto r = runChild(args, {}, false);
                rc = r.code;
                dbg("launch-client rc=" + std::to_string(rc));
            }
        }
        if (rc != 0 && nsenterWorks(pid)) {
            std::string envFile = std::string("/tmp/thprac_xattach_env_") + std::to_string(getpid());
            FILE* f = fopen(envFile.c_str(), "wb");
            if (f) {
                for (const auto& e : env) { fwrite(e.c_str(), 1, e.size(), f); fputc(0, f); }
                fclose(f);
                std::vector<std::string> args = {"/usr/bin/nsenter", "-t", std::to_string(pid),
                    "-U", "--preserve-credentials", "-m", "-r", "-w", "--", selfExe,
                    "run", envFile, kLogPath, loader, launcherExe, "--attach"};
                auto r = runChild(args, {}, true);
                rc = r.code;
                dbg("nsenter rc=" + std::to_string(rc));
                unlink(envFile.c_str());
            }
        }
    } else {
        auto r = runChild({loader, launcherExe, "--attach"}, env, true);
        rc = r.code;
        dbg("direct rc=" + std::to_string(rc));
    }
    emit("RC " + std::to_string(rc) + "\n");
    emit("DONE\n");
}

static std::string hexDecode(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back(char((hi << 4) | lo));
    }
    return out;
}
static std::vector<std::string> tokenize(const std::string& cmd) {
    std::vector<std::string> args;
    std::string cur;
    bool have = false, quote = false;
    char q = 0;
    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (quote) {
            if (c == q) { quote = false; }
            else cur.push_back(c);
            continue;
        }
        if (c == '\'' || c == '"') { quote = true; q = c; have = true; continue; }
        if (c == '\\' && i + 1 < cmd.size()) { cur.push_back(cmd[++i]); have = true; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (have) { args.push_back(cur); cur.clear(); have = false; }
            continue;
        }
        cur.push_back(c); have = true;
    }
    if (have) args.push_back(cur);
    return args;
}
static std::vector<std::string> splitAlternatives(const std::string& cmd) {
    std::vector<std::string> result;
    size_t start = 0;
    char quote = 0;
    for (size_t i = 0; i + 1 < cmd.size(); ++i) {
        char c = cmd[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '\'' || c == '"') { quote = c; continue; }
        if (c == '|' && cmd[i + 1] == '|') {
            result.push_back(cmd.substr(start, i - start));
            start = i + 2;
            ++i;
        }
    }
    result.push_back(cmd.substr(start));
    return result;
}
static int execAlternative(const std::string& cmd) {
    auto args = tokenize(cmd);
    if (args.empty()) return 1;
    std::vector<char*> av;
    for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    execvp(args[0].c_str(), av.data());
    dbg("execvp failed: " + args[0] + " errno=" + std::to_string(errno));
    return 127;
}
static int doCommand(const std::string& hex, const std::string& mode, int timeoutMs) {
    std::string command = hexDecode(hex);
    dbg("cmd mode=" + mode + " command=" + command);
    unsetenv("LD_PRELOAD");
    unsetenv("LD_PRELOAD_64");
    if (mode == "async") {
        pid_t child = fork();
        if (child == 0) _exit(execAlternative(command));
        return 0;
    }
    auto alternatives = splitAlternatives(command);
    int maxWait = timeoutMs > 0 ? timeoutMs : 300000;
    for (const auto& alt : alternatives) {
        pid_t child = fork();
        if (child == 0) _exit(execAlternative(alt));
        int status = 0;
        int waited = 0;
        bool exited = false;
        while (waited < maxWait) {
            pid_t r = waitpid(child, &status, WNOHANG);
            if (r == child) { exited = true; break; }
            if (r < 0 && errno == ECHILD) { exited = true; break; }
            usleep(50000);
            waited += 50;
        }
        if (!exited) {
            // Leave the long-running command (e.g. a game launcher) alive.
            return 0;
        }
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        dbg("cmd exit code=" + std::to_string(code));
        if (code == 0) return 0;
    }
    return 1;
}

static void doUpdate(const std::string& exeHex, const std::string& stagedHex, const std::string& oldHex, int oldPid) {
    std::string exe = hexDecode(exeHex), staged = hexDecode(stagedHex), old = hexDecode(oldHex);
    pid_t parent = oldPid > 0 ? oldPid : getppid();
    std::string loaderExe = readLink("/proc/" + std::to_string(parent) + "/exe");
    std::string dir = loaderExe.substr(0, loaderExe.find_last_of('/'));
    std::string loader;
    for (const auto& rel : {"/wine64", "/wine", "/../../../bin/wine64", "/../../../bin/wine"}) {
        std::string p = dir + rel;
        if (executable(p)) { loader = p; break; }
    }
    dbg("update wait for parent=" + std::to_string(parent) + " loader=" + loader);
    for (int i = 0; i < 600; ++i) {
        if (kill(parent, 0) != 0 && errno == ESRCH) break;
        usleep(50000);
    }
    usleep(200000);
    unlink(old.c_str());
    if (rename(exe.c_str(), old.c_str()) != 0 || rename(staged.c_str(), exe.c_str()) != 0) {
        dbg("update rename failed errno=" + std::to_string(errno));
        // Roll back if the second rename failed.
        if (access(exe.c_str(), F_OK) != 0 && access(old.c_str(), F_OK) == 0)
            rename(old.c_str(), exe.c_str());
        _exit(1);
    }
    dbg("update swapped: " + exe);
    if (loader.empty()) loader = "wine";
    std::vector<std::string> args = {loader, exe};
    std::vector<char*> av;
    for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    execvp(loader.c_str(), av.data());
    dbg("update exec failed errno=" + std::to_string(errno));
    _exit(1);
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    std::string mode = argv[1];
    if (mode == "run") {
        if (argc < 6) return 2;
        std::string envText;
        readAll(argv[2], envText);
        redirectLog(argv[3]);
        std::vector<std::string> env;
        for (const auto& line : split(envText, '\0')) if (!line.empty()) env.push_back(line);
        std::vector<std::string> args;
        for (int i = 4; i < argc; ++i) args.push_back(argv[i]);
        auto r = runChild(args, env, true);
        dbg("run rc=" + std::to_string(r.code));
        return r.code;
    }
    if (mode == "cmd") {
        if (argc < 6) return 2;
        redirectLog(argv[5]);
        return doCommand(argv[2], argv[3], atoi(argv[4]));
    }
    if (mode == "update") {
        if (argc < 5) return 2;
        redirectLog(argv[2]);
        doUpdate(argv[3], argv[4], argc > 5 ? argv[5] : "", argc > 6 ? atoi(argv[6]) : 0);
        return 1;
    }
    std::string selfExe = argv[2];
    kOutPath = argv[3];
    redirectLog(argv[4]);
    bool hostDispatch = getenv("PRESSURE_VESSEL_FILENAMES") || getenv("STEAM_COMPAT_DATA_PATH");
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--host") hostDispatch = true;
    if (!hostDispatch && getenv("WINEPREFIX")) {
        std::string pfx = getenv("WINEPREFIX");
        hostDispatch = pfx.find("/compatdata/") != std::string::npos ||
                       pfx.find("\\compatdata\\") != std::string::npos;
    }
    if (hostDispatch && !getenv("SELF_HOST") && mode != "run") {
        std::string lclient = findLaunchClient();
        if (lclient.empty()) {
            dbg("no launch client for host dispatch");
            emit("ERR no launch client\nRC 1\nDONE\n");
            return 1;
        }
        std::vector<std::string> args = {lclient, "--alongside-steam", "--directory=/",
            "--env=SELF_HOST=1", "--", selfExe};
        for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
        std::vector<char*> av;
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        dbg("host dispatch via " + lclient);
        execv(lclient.c_str(), av.data());
        dbg("host dispatch execv failed errno=" + std::to_string(errno));
        emit("ERR host dispatch failed\nRC 1\nDONE\n");
        return 1;
    }
    dbg("mode=" + mode + " hostDispatch=" + std::to_string(hostDispatch ? 1 : 0) +
        " self=" + selfExe);
    if (mode == "scan") {
        doScan();
    } else if (mode == "apply" && argc >= 8) {
        doApply(selfExe, argv[5], atoi(argv[6]), std::string(argv[7]) == "1");
    } else {
        emit("ERR bad mode/args\nDONE\n");
        return 2;
    }
    return 0;
}
