/*
 * thprac-injector: ptrace-based PE module loader for Wine processes.
 *
 * Loads a Windows PE module (DLL or EXE used as a library) into a running
 * Wine process by attaching with ptrace and remotely calling LoadLibraryW.
 * This works across Wine prefixes and pressure-vessel containers as long as
 * the caller is allowed to ptrace the target (kernel.yama.ptrace_scope and
 * cap_sys_ptrace decide that; see the EPERM hint in the error output).
 *
 * Usage:
 *   thprac-injector --pid <pid> --dll 'Z:\path\to\module.dll'
 *   thprac-injector --spawn 'wine64 /path/target.exe' --dll ...   (test mode:
 *       spawns the target as a child, so ptrace is allowed even under
 *       yama ptrace_scope=1)
 *
 * Exit codes:
 *   0  module loaded
 *   1  usage / argument error
 *   2  cannot inspect the target (not found, not a supported Wine process)
 *   3  ptrace denied (hint: sudo setcap cap_sys_ptrace=ep <this file>)
 *   4  32-bit target (only x86_64 processes are supported)
 *   5  kernel32/ntdll not loaded yet in the target (try again shortly)
 *   6  LoadLibraryW failed in the target (bad path or module error)
 *   7  other injector failure
 *
 * Diagnostics go to stderr. With --result <path> a final status line
 * ("ok" / "err ...") is written to that file (DOS Z:\ or Unix path).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAP_PRIVATE_ 0x02
#define MAP_ANONYMOUS_ 0x20
#define PROT_RW (PROT_READ | PROT_WRITE)
#define NR_mmap 9

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void say_errno(const char* what) {
    say("error: %s: %s", what, strerror(errno));
}

/* ------------------------------------------------------------------ */
/* target memory                                                       */
/* ------------------------------------------------------------------ */

static ssize_t read_mem(pid_t pid, uint64_t addr, void* buf, size_t len) {
    struct iovec local = { buf, len };
    struct iovec remote = { (void*)(uintptr_t)addr, len };
    ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    if (n >= 0) return n;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t got = pread(fd, buf, len, (off_t)addr);
    close(fd);
    return got;
}

static ssize_t write_mem(pid_t pid, uint64_t addr, const void* buf, size_t len) {
    struct iovec local = { (void*)buf, len };
    struct iovec remote = { (void*)(uintptr_t)addr, len };
    ssize_t n = process_vm_writev(pid, &local, 1, &remote, 1, 0);
    if (n >= 0) return n;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t wrote = pwrite(fd, buf, len, (off_t)addr);
    close(fd);
    return wrote;
}

static int write_mem_ok(pid_t pid, uint64_t addr, const void* buf, size_t len) {
    return write_mem(pid, addr, buf, len) == (ssize_t)len;
}

static int write_cstr16(pid_t pid, uint64_t addr, const uint16_t* s, size_t words) {
    return write_mem(pid, addr, s, words * 2) == (ssize_t)(words * 2);
}

/* UTF-8 -> UTF-16LE. Returns number of uint16 words written (including L'\0'). */
static size_t utf8_to_utf16(const char* src, uint16_t* dst, size_t max_words) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 2 < max_words;) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; }
        else break;
        int ok = 1;
        for (int k = 0; k < extra; k++) {
            unsigned char cc = (unsigned char)src[i + 1 + k];
            if ((cc & 0xc0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (!ok) break;
        i += 1 + extra;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            dst[o++] = (uint16_t)(0xd800 + (cp >> 10));
            dst[o++] = (uint16_t)(0xdc00 + (cp & 0x3ff));
        } else {
            dst[o++] = (uint16_t)cp;
        }
    }
    dst[o] = 0;
    return o + 1;
}

/* ------------------------------------------------------------------ */
/* maps                                                                */
/* ------------------------------------------------------------------ */

struct region {
    uint64_t start, end;
    char perms[8];
    uint64_t offset;
    char path[1024];
};

static int read_maps(pid_t pid, struct region* out, int max) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[2400];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < max) {
        struct region r;
        memset(&r, 0, sizeof(r));
        char* dash = strchr(line, '-');
        if (!dash) continue;
        *dash = 0;
        r.start = strtoull(line, NULL, 16);
        char* sp = strchr(dash + 1, ' ');
        if (!sp) continue;
        r.end = strtoull(dash + 1, NULL, 16);
        char* p = sp + 1;
        char* q = strchr(p, ' ');
        if (!q) continue;
        size_t pl = (size_t)(q - p);
        if (pl >= sizeof(r.perms)) pl = sizeof(r.perms) - 1;
        memcpy(r.perms, p, pl);
        r.perms[pl] = 0;
        p = q + 1;
        q = strchr(p, ' ');
        if (!q) continue;
        r.offset = strtoull(p, NULL, 16);
        p = q + 1;
        while (*p == ' ') p++;
        if (*p && *p != '\n') {
            size_t plen = strlen(p);
            while (plen && (p[plen - 1] == '\n' || p[plen - 1] == ' ')) plen--;
            if (plen >= sizeof(r.path)) plen = sizeof(r.path) - 1;
            memcpy(r.path, p, plen);
            r.path[plen] = 0;
        }
        out[n++] = r;
    }
    fclose(f);
    return n;
}

/* Lowest mapping whose path contains `needle`. The PE headers live at the
 * lowest mapping of a builtin module (which may not be executable). */
static uint64_t module_base(const struct region* regions, int n, const char* needle) {
    uint64_t best = 0;
    for (int i = 0; i < n; i++) {
        if (!regions[i].path[0]) continue;
        if (!strstr(regions[i].path, needle)) continue;
        if (!best || regions[i].start < best) best = regions[i].start;
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* PE exports (PE32+ images in target memory)                          */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint16_t rd16(const uint8_t* b) {
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint64_t pe_export(pid_t pid, uint64_t base, const char* name) {
    uint8_t dos[64];
    if (read_mem(pid, base, dos, sizeof(dos)) != sizeof(dos)) return 0;
    if (dos[0] != 'M' || dos[1] != 'Z') return 0;
    uint32_t e_lfanew = rd32(dos + 0x3c);
    if (!e_lfanew || e_lfanew > 0x1000) return 0;
    uint8_t nt[256];
    if (read_mem(pid, base + e_lfanew, nt, sizeof(nt)) != sizeof(nt)) return 0;
    if (nt[0] != 'P' || nt[1] != 'E') return 0;
    uint16_t magic = rd16(nt + 24);
    if (magic != 0x20b) return 0; /* PE32+ only */
    const uint8_t* dd = nt + 24 + 112; /* data directory 0 = exports */
    uint32_t exp_rva = rd32(dd), exp_size = rd32(dd + 4);
    if (!exp_rva || !exp_size) return 0;
    uint8_t exp[40];
    if (read_mem(pid, base + exp_rva, exp, sizeof(exp)) != sizeof(exp)) return 0;
    uint32_t names_rva = rd32(exp + 32);
    uint32_t funcs_rva = rd32(exp + 28);
    uint32_t ords_rva = rd32(exp + 36);
    uint32_t n_names = rd32(exp + 24);
    if (!n_names || n_names > 200000) return 0;
    uint32_t* name_rvas = malloc(n_names * 4);
    if (!name_rvas) return 0;
    if (read_mem(pid, base + names_rva, name_rvas, n_names * 4) != (ssize_t)(n_names * 4)) {
        free(name_rvas);
        return 0;
    }
    size_t nlen = strlen(name);
    uint64_t found = 0;
    for (uint32_t i = 0; i < n_names && !found; i++) {
        char nbuf[80];
        if (read_mem(pid, base + name_rvas[i], nbuf, sizeof(nbuf)) != sizeof(nbuf)) continue;
        nbuf[sizeof(nbuf) - 1] = 0;
        if (strncmp(nbuf, name, nlen) != 0 || nbuf[nlen] != 0) continue;
        uint16_t ord = 0;
        if (read_mem(pid, base + ords_rva + i * 2, &ord, 2) != 2) break;
        uint32_t fn_rva = 0;
        if (read_mem(pid, base + funcs_rva + ord * 4, &fn_rva, 4) != 4) break;
        found = base + fn_rva;
    }
    free(name_rvas);
    return found;
}

static uint32_t pe_entry_rva(pid_t pid, uint64_t base) __attribute__((unused));
static uint32_t pe_entry_rva(pid_t pid, uint64_t base) {
    uint8_t dos[64];
    if (read_mem(pid, base, dos, sizeof(dos)) != sizeof(dos)) return 0;
    uint32_t e_lfanew = rd32(dos + 0x3c);
    uint8_t nt[256];
    if (read_mem(pid, base + e_lfanew, nt, sizeof(nt)) != sizeof(nt)) return 0;
    return rd32(nt + 24 + 16); /* OptionalHeader.AddressOfEntryPoint */
}

/* ------------------------------------------------------------------ */
/* ptrace                                                              */
/* ------------------------------------------------------------------ */

static int list_tids(pid_t pid, int* tids, int max) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR* d = opendir(path);
    if (!d) return 0;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        tids[n++] = atoi(e->d_name);
    }
    closedir(d);
    return n;
}

static int get_regs(pid_t tid, struct user_regs_struct* r) {
    struct iovec iov = { r, sizeof(*r) };
    return ptrace(PTRACE_GETREGSET, tid, (void*)(uintptr_t)1 /*NT_PRSTATUS*/, &iov) == 0 ? 0 : -1;
}

static int set_regs(pid_t tid, const struct user_regs_struct* r) {
    struct iovec iov = { (void*)r, sizeof(*r) };
    return ptrace(PTRACE_SETREGSET, tid, (void*)(uintptr_t)1, &iov) == 0 ? 0 : -1;
}

/* Wait until tid reports a stop. Returns the stop signal (or 0 on exit,
 * -1 on error). */
static int wait_stop_sig(pid_t tid) {
    for (;;) {
        int status = 0;
        pid_t got = waitpid(tid, &status, __WALL);
        if (got == tid) {
            if (WIFSTOPPED(status)) return WSTOPSIG(status);
            if (WIFEXITED(status) || WIFSIGNALED(status)) return 0;
            continue;
        }
        if (got == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
    }
}

/* Wait until tid reports a stop. Returns 1 on stop, 0 on exit, -1 on error. */
static int wait_stop(pid_t tid) {
    int sig = wait_stop_sig(tid);
    return sig > 0 ? 1 : sig;
}

/* Seize one thread and interrupt it. Returns 1 on stop, 0/−1 on failure. */
static int seize_thread(pid_t tid) {
    if (ptrace(PTRACE_SEIZE, tid, 0, 0) == -1) return -1;
    if (ptrace(PTRACE_INTERRUPT, tid, 0, 0) == -1 && errno != ESRCH) {
        ptrace(PTRACE_DETACH, tid, 0, 0);
        return -1;
    }
    return wait_stop(tid);
}

/* Find a "syscall" (0f 05) instruction in the target's executable regions. */
static uint64_t find_syscall_instr(pid_t pid, const struct region* regions, int n) {
    /* Prefer big executable file-backed regions (wine core), then any exec region. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            if (!strchr(regions[i].perms, 'x')) continue;
            int is_file = regions[i].path[0] == '/';
            if (pass == 0 && !is_file) continue;
            if (pass == 1 && is_file) continue;
            uint64_t span = regions[i].end - regions[i].start;
            if (span > 8u * 1024 * 1024) span = 8u * 1024 * 1024; /* cap scan size */
            static uint8_t buf[256 * 1024];
            uint64_t scan_end = regions[i].start + span;
            for (uint64_t a = regions[i].start; a < scan_end; a += sizeof(buf) - 8) {
                size_t chunk = sizeof(buf) - 8;
                if (a + chunk > scan_end) chunk = (size_t)(scan_end - a);
                if (read_mem(pid, a, buf, chunk) != (ssize_t)chunk) break;
                for (size_t k = 0; k + 1 < chunk; k++) {
                    if (buf[k] == 0x0f && buf[k + 1] == 0x05) {
                        /* Skip the first few: they may sit inside prologue data
                         * of the preloader entry; any 0f 05 works when we set
                         * rip and single-step, so take it. */
                        return a + k;
                    }
                }
            }
        }
    }
    return 0;
}

/* Find a 0xCC (int3) byte in an executable region for the return trampoline. */
static uint64_t find_int3(pid_t pid, const struct region* regions, int n) {
    static uint8_t buf[16 * 1024];
    for (int i = 0; i < n; i++) {
        if (!strchr(regions[i].perms, 'x')) continue;
        uint64_t span = regions[i].end - regions[i].start;
        if (span > 1024u * 1024) span = 1024u * 1024;
        uint64_t scan_end = regions[i].start + span;
        for (uint64_t a = regions[i].start; a < scan_end; a += sizeof(buf)) {
            size_t chunk = sizeof(buf);
            if (a + chunk > scan_end) chunk = (size_t)(scan_end - a);
            if (read_mem(pid, a, buf, chunk) != (ssize_t)chunk) break;
            for (size_t k = 0; k < chunk; k++)
                if (buf[k] == 0xcc) return a + k;
        }
    }
    return 0;
}

/* Run one remote syscall on tid via the instruction at `sysaddr`.
 * regs must already contain the desired arguments and rax=nr; rip is set here.
 * Returns the syscall result (rax) or (uint64_t)-1 on failure. */
static int remote_syscall(pid_t tid, uint64_t sysaddr, struct user_regs_struct* r, uint64_t* result) {
    uint64_t nr = r->rax;
    r->rip = sysaddr;
    r->eflags &= ~0x100; /* clear TF */
    if (set_regs(tid, r) < 0) return -1;
    /* PTRACE_SYSCALL stops at syscall entry and exit; step until the exit stop. */
    for (int step = 0; step < 4; step++) {
        if (ptrace(PTRACE_SYSCALL, tid, 0, 0) == -1) return -1;
        int sig = wait_stop_sig(tid);
        if (sig <= 0) return -1;
        struct user_regs_struct after;
        if (get_regs(tid, &after) < 0) return -1;
        if (after.rip == sysaddr + 2 && after.rax != nr) {
            *result = after.rax;
            return 0;
        }
        if (getenv("THPRAC_INJ_DEBUG")) {
            say("remote_syscall: step %d sig=%d rip=%llx rax=%llx", step, sig,
                (unsigned long long)after.rip, (unsigned long long)after.rax);
        }
        if (sig != SIGTRAP && sig != (SIGTRAP | 0x80)) {
            /* unrelated signal: pass it through and retry */
            if (ptrace(PTRACE_SYSCALL, tid, 0, (void*)(uintptr_t)sig) == -1) return -1;
            wait_stop_sig(tid);
        }
    }
    say("remote_syscall: syscall did not complete");
    return -1;
}

/* Call a Win64 function on tid. Sets rip=function, arguments via regs/stack,
 * return address points to an int3 so we regain control. On return, rax holds
 * the result and regs are left stopped at the int3. */
static int hijack_call(pid_t tid, uint64_t func, uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9,
    const uint64_t* stack_args, int n_stack_args, uint64_t ret_site, struct user_regs_struct* saved,
    uint64_t* scratch_rsp, uint64_t* result) {
    struct user_regs_struct r = *saved;
    /* Use the thread's real stack (within its TEB stack limits, as Wine's SEH
     * and loader checks require). The thread may have been stopped on Wine's
     * syscall stack, so read the limits from the TEB instead of trusting rsp. */
    uint64_t teb = r.gs_base;
    uint64_t stack_base = 0, stack_limit = 0;
    if (teb && read_mem(tid ? tid : tid, teb + 0x08, &stack_base, 8) == 8 &&
        read_mem(tid, teb + 0x10, &stack_limit, 8) == 8 && stack_base > stack_limit) {
        /* pick a spot just under the stack base */
        uint64_t rsp2 = stack_base - 0x2000;
        if (rsp2 <= stack_limit) rsp2 = stack_limit + 0x1000;
        r.rsp = (rsp2 & ~0xfULL) + 8;
    } else {
        r.rsp = ((r.rsp - 0x400) & ~0xfULL) + 8;
    }
    uint64_t rsp = r.rsp;
    say("hijack: teb=%llx stack=[%llx..%llx] rsp=%llx", (unsigned long long)teb,
        (unsigned long long)stack_limit, (unsigned long long)stack_base, (unsigned long long)rsp);
    uint64_t slot[8] = { 0 };
    /* [rsp] = return address */
    slot[0] = ret_site;
    if (write_mem(tid, rsp, slot, 8) != 8) return -1;
    for (int i = 0; i < n_stack_args; i++) {
        uint64_t v = stack_args[i];
        /* 5th argument lives at rsp+0x28 (after shadow space) */
        if (write_mem(tid, rsp + 0x28 + (uint64_t)i * 8, &v, 8) != 8) return -1;
    }
    (void)scratch_rsp;
    r.rsp = rsp;
    r.rip = func;
    r.rax = 0;
    r.rcx = rcx;
    r.rdx = rdx;
    r.r8 = r8;
    r.r9 = r9;
    r.eflags &= ~0x100;
    if (set_regs(tid, &r) < 0) return -1;
    /* Run until the thread hits the return trampoline. First-chance exceptions
     * inside the loader are passed through so Wine's SEH can handle them. */
    for (;;) {
        if (ptrace(PTRACE_CONT, tid, 0, 0) == -1) {
            say_errno("hijack: CONT");
            return -1;
        }
        int sig = wait_stop_sig(tid);
        if (sig <= 0) {
            say("hijack: wait returned %d (%s)", sig, sig == 0 ? "thread exited" : "error");
            return -1;
        }
        struct user_regs_struct after;
        if (get_regs(tid, &after) < 0) {
            say_errno("hijack: GETREGS");
            return -1;
        }
        /* After an int3 fires, rip points one byte past the int3. */
        if (after.rip == ret_site || after.rip == ret_site + 1) {
            *result = after.rax;
            return 0;
        }
        say("hijack: pass-through stop sig=%d rip=%llx", sig, (unsigned long long)after.rip);
        if (ptrace(PTRACE_CONT, tid, 0, (void*)(uintptr_t)sig) == -1) {
            say_errno("hijack: CONT(sig)");
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* ELF class / wine detection                                          */
/* ------------------------------------------------------------------ */

/* Wine processes expose the PE exe as /proc/pid/exe, so detect Wine by the
 * maps signatures instead: the builtin PE ntdll.dll mapping tells both that
 * this is Wine and which PE architecture the process runs.
 *
 * Modern Wine runs 32-bit games under wow64: one 64-bit process containing
 * BOTH the i386-windows PE parts (the actual game) and the x86_64-windows PE
 * parts (the 64-bit host side, with its own ntdll/kernel32). In that case we
 * drive the 64-bit side: load a 64-bit loader DLL, which starts the regular
 * 32-bit thprac helper in the same container.
 */
static int target_arch(pid_t pid, int* unsupported32, int* wow64) {
    *unsupported32 = 0;
    *wow64 = 0;
    static struct region regions[2048];
    int n = read_maps(pid, regions, 2048);
    if (n <= 0) return 0;
    int has64 = 0, has32 = 0, wine = 0;
    for (int i = 0; i < n; i++) {
        if (!regions[i].path[0]) continue;
        if (strstr(regions[i].path, "/x86_64-windows/ntdll.dll")) has64 = 1;
        if (strstr(regions[i].path, "/i386-windows/ntdll.dll")) has32 = 1;
        if (strstr(regions[i].path, "wine-preloader")) wine = 1;
    }
    if (has64) {
        *wow64 = has32;
        return 1;
    }
    if (has32) {
        /* Old-style 32-bit Wine process: the whole process is ELF32. */
        *unsupported32 = 1;
        return 0;
    }
    (void)wine;
    return 0; /* not ready yet or not a Wine process */
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void) {
    say("usage: thprac-injector --pid <pid> --dll <DOS path> [--wait <sec>] [--sig] [--result <path>]\n"
        "       thprac-injector --find <exe[,exe2...]> --dll <DOS path> [--wait <sec>] [--sig] [--result <path>]\n"
        "       thprac-injector --spawn '<command>' --dll <DOS path>   (test mode)");
}

/* Find a Wine process whose command line contains one of the given exe
 * basenames. Returns the pid or 0. Reading /proc requires no special rights,
 * so this works before any ptrace attachment. */
static pid_t find_game_pid(const char* names) {
    DIR* d = opendir("/proc");
    if (!d) return 0;
    pid_t best = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        pid_t c = atoi(e->d_name);
        if (c <= 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", c);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char buf[1024] = {};
        ssize_t got = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (got <= 0) continue;
        for (char* z = buf; z < buf + got; ++z)
            if (*z == 0) *z = ' ';
        int matched = 0;
        const char* p = names;
        for (;;) {
            const char* comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            if (len && memmem(buf, (size_t)got, p, len)) {
                matched = 1;
                break;
            }
            if (!comma) break;
            p = comma + 1;
        }
        if (!matched) continue;
        int unsupported32 = 0, wow64 = 0;
        if (!target_arch(c, &unsupported32, &wow64)) continue;
        best = c;
        break;
    }
    closedir(d);
    return best;
}

/* Read the target's main image base from its maps (lowest file-backed
 * mapping whose path looks like the game exe). Requires no ptrace. */
static uint64_t target_image_base(pid_t pid) {
    static struct region regs[4096];
    int n = read_maps(pid, regs, 4096);
    uint64_t best = 0;
    for (int i = 0; i < n; i++) {
        if (!regs[i].path[0] || !strstr(regs[i].path, ".exe")) continue;
        if (strstr(regs[i].path, "/x86_64-windows/") || strstr(regs[i].path, "/i386-windows/")) continue;
        if (!best || regs[i].start < best) best = regs[i].start;
    }
    return best;
}

/* 'PRAC' signature written by thprac when a game is already applied. */
static int target_already_applied(pid_t pid) {
    uint64_t base = target_image_base(pid);
    if (!base) return 0;
    uint32_t e_lfanew = 0;
    if (read_mem(pid, base + 0x3c, &e_lfanew, 4) != 4 || e_lfanew < 4) return 0;
    uint32_t sig = 0;
    if (read_mem(pid, base + e_lfanew - 4, &sig, 4) != 4) return 0;
    return sig == 0x43524150UL;
}

/* DOS (Z:\...) -> Unix path. Also accepts plain Unix paths. */
static void dos_to_unix(const char* dos, char* out, size_t outsz) {
    if (!dos || !dos[0]) { if (outsz) out[0] = 0; return; }
    if (dos[0] == '/') { snprintf(out, outsz, "%s", dos); return; }
    if (((dos[0] >= 'A' && dos[0] <= 'Z') || (dos[0] >= 'a' && dos[0] <= 'z')) && dos[1] == ':') {
        char drive = (char)(dos[0] | 0x20);
        const char* rest = dos + 2;
        while (*rest == '\\' || *rest == '/') ++rest;
        if (drive == 'z') {
            snprintf(out, outsz, "/%s", rest);
        } else {
            snprintf(out, outsz, "/mnt/%c/%s", drive, rest);
        }
        for (char* p = out; *p; ++p)
            if (*p == '\\') *p = '/';
        return;
    }
    snprintf(out, outsz, "%s", dos);
}

static const char* g_result_path = NULL;

static void write_result_file(int code) {
    if (!g_result_path || !g_result_path[0]) return;
    char unixpath[4096];
    dos_to_unix(g_result_path, unixpath, sizeof(unixpath));
    if (!unixpath[0]) return;
    const char* text;
    switch (code) {
    case 0: text = "ok"; break;
    case 3: text = "err ptrace-denied (one-time fix: sudo setcap cap_sys_ptrace=ep <injector>)"; break;
    case 4: text = "err 32-bit-wine-unsupported"; break;
    case 5: text = "err loader-not-ready"; break;
    case 6: text = "err module-load-failed"; break;
    case 2: text = "err target-not-a-supported-wine-process"; break;
    default: text = "err injector-failure"; break;
    }
    FILE* f = fopen(unixpath, "w");
    if (!f) {
        say("cannot write result file %s: %s", unixpath, strerror(errno));
        return;
    }
    fprintf(f, "%s\n", text);
    fclose(f);
    say("result written to %s", unixpath);
}

static int run(int argc, char** argv);

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int pid_matches_target(pid_t pid, const char* exe_token, int* unsupported32, int* wow64) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[1024] = {};
    ssize_t got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0) return 0;
    /* The wine process command line shows the DOS form (Z:\...). Match on the
     * basename in either form, and require the game image to actually be
     * mapped (this skips wine's intermediate start.exe stages). */
    const char* base = strrchr(exe_token, '/');
    base = base ? base + 1 : exe_token;
    if (!strstr(buf, base)) return 0;
    /* Prefer processes from our own prefix (spawn mode). */
    if (getenv("WINEPREFIX")) {
        char envpath[64];
        snprintf(envpath, sizeof(envpath), "/proc/%d/environ", pid);
        int efd = open(envpath, O_RDONLY);
        if (efd >= 0) {
            static char ebuf[32768];
            size_t eoff = 0;
            for (;;) {
                if (eoff >= sizeof(ebuf) - 1) break;
                ssize_t cg = read(efd, ebuf + eoff, sizeof(ebuf) - 1 - eoff);
                if (cg <= 0) break;
                eoff += (size_t)cg;
            }
            close(efd);
            const char* wp = getenv("WINEPREFIX");
            if (!eoff || !wp || !strstr(ebuf, wp)) return 0;
        }
    }
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    static char maps[262144];
    size_t off = 0;
    for (;;) {
        if (off >= sizeof(maps) - 1) break;
        ssize_t cg = read(fd, maps + off, sizeof(maps) - 1 - off);
        if (cg <= 0) break;
        off += (size_t)cg;
    }
    close(fd);
    maps[off] = 0;
    if (!off) return 0;
    if (!strstr(maps, base)) {
        if (getenv("THPRAC_INJ_DEBUG")) say("spawn: pid %d cmdline ok but image not in maps", pid);
        return 0;
    }
    int r = target_arch(pid, unsupported32, wow64);
    if (getenv("THPRAC_INJ_DEBUG")) say("spawn: pid %d image mapped, arch=%d unsupported32=%d wow64=%d", pid, r, *unsupported32, *wow64);
    return r;
}

static pid_t spawn_target(const char* cmdline, int wait_secs) {
    /* "exec" makes the shell replace itself with the command, so the pid we
     * track is the one that eventually execs wine's preloader. */
    char* cmd = malloc(strlen(cmdline) + 8);
    if (!cmd) return -1;
    sprintf(cmd, "exec %s", cmdline);
    pid_t pid = fork();
    if (pid < 0) {
        free(cmd);
        return -1;
    }
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    free(cmd);
    /* Wait for the exec-stop. */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        say_errno("spawn: waitpid");
        return -1;
    }
    if (!WIFSTOPPED(status)) {
        say("spawn: no exec-stop (status=%x exited=%d code=%d sig=%d)", status, WIFEXITED(status),
            WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        return -1;
    }
    say("spawn: exec-stop ok");
    /* Let it run. Wine's launcher daemonizes the game (start.exe /exec forks
     * and exits), so keep the launcher untraced and scan /proc for whichever
     * process ends up running the requested image. */
    ptrace(PTRACE_DETACH, pid, 0, 0);

    /* The game execs in a descendant process; find it via its command line
     * and mapped image. */
    const char* exe_token = strrchr(cmdline, ' ');
    exe_token = exe_token ? exe_token + 1 : cmdline;
    uint64_t deadline = now_ms() + (uint64_t)wait_secs * 4 * 1000;
    for (;;) {
        int unsupported32 = 0, wow64 = 0;
        if (pid_matches_target(pid, exe_token, &unsupported32, &wow64)) {
            say("spawn: game process %d found%s", pid, wow64 ? " (wow64)" : "");
            return pid;
        }
        char dir[16];
        snprintf(dir, sizeof(dir), "/proc");
        DIR* d = opendir(dir);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d))) {
                if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
                pid_t c = atoi(e->d_name);
                if (c == pid) continue;
                unsupported32 = wow64 = 0;
                if (pid_matches_target(c, exe_token, &unsupported32, &wow64)) {
                    closedir(d);
                    say("spawn: game process %d found%s", c, wow64 ? " (wow64)" : "");
                    return c;
                }
            }
            closedir(d);
        }
        if (now_ms() > deadline) {
            say("spawn: timeout waiting for the game process");
            return -2;
        }
        usleep(100000);
    }
}

/* Is rip inside one of the target's PE module ranges? A thread stopped there
 * is in plain user mode: it has no pending wineserver request, so running
 * LoadLibraryW (which performs its own wineserver transactions) cannot mix
 * request/reply streams on its socket. */
struct range { uint64_t lo, hi; };

static int rip_in_pe(uint64_t rip, const struct range* pes, int n) {
    for (int i = 0; i < n; i++)
        if (rip >= pes[i].lo && rip < pes[i].hi) return 1;
    return 0;
}

static int collect_pe_ranges(const struct region* regions, int n, struct range* pes, int max) {
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        if (!strstr(regions[i].path, "/x86_64-windows/")) continue;
        /* merge with the previous range if adjacent */
        if (count && pes[count - 1].hi == regions[i].start) {
            pes[count - 1].hi = regions[i].end;
            continue;
        }
        pes[count].lo = regions[i].start;
        pes[count].hi = regions[i].end;
        count++;
    }
    return count;
}

/* Seize one thread that is not currently inside a syscall (hijacking a thread
 * in syscall-restart state can corrupt its restart bookkeeping). Returns the
 * stopped tid with regs saved, or 0. */
static pid_t seize_worker(pid_t pid, struct user_regs_struct* saved, const struct range* pes, int n_pes) {
    int tids[256];
    int n = list_tids(pid, tids, 256);
    if (n <= 0) return 0;
    for (int attempt = 0; attempt < n; attempt++) {
        if (seize_thread(tids[attempt]) != 1) {
            if (errno == EPERM) return (pid_t)-1;
            continue;
        }
        struct user_regs_struct r;
        if (get_regs(tids[attempt], &r) < 0) {
            ptrace(PTRACE_DETACH, tids[attempt], 0, 0);
            continue;
        }
        if ((r.cs & 0xffff) != 0x33 || !rip_in_pe(r.rip, pes, n_pes)) {
            /* 32-bit mode or inside a syscall / unix code: not a safe hijack target. */
            ptrace(PTRACE_DETACH, tids[attempt], 0, 0);
            continue;
        }
        *saved = r;
        return tids[attempt];
    }
    return 0;
}

int main(int argc, char** argv) {
    int code = run(argc, argv);
    write_result_file(code);
    return code;
}

static int run(int argc, char** argv) {
    pid_t pid = 0;
    const char* dll = NULL;
    const char* spawn = NULL;
    const char* find = NULL;
    int wait_secs = 5;
    int spawned = 0;
    int dry = 0;
    int sig = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pid") && i + 1 < argc) pid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
        else if (!strcmp(argv[i], "--spawn") && i + 1 < argc) spawn = argv[++i];
        else if (!strcmp(argv[i], "--find") && i + 1 < argc) find = argv[++i];
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait_secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--result") && i + 1 < argc) g_result_path = argv[++i];
        else if (!strcmp(argv[i], "--sig")) sig = 1;
        else if (!strcmp(argv[i], "--dry")) dry = 1;
        else { usage(); return 1; }
    }
    if (!dll || (!pid && !spawn && !find)) {
        usage();
        return 1;
    }
    if (spawn) {
        pid = spawn_target(spawn, wait_secs);
        if (pid <= 0) {
            say_errno("spawn");
            return 7;
        }
        spawned = 1;
        say("spawned target pid %d", pid);
    }
    if (find && !pid) {
        /* Poll for the game process (it may still be starting). */
        uint64_t deadline = now_ms() + (uint64_t)wait_secs * 4 * 1000;
        for (;;) {
            pid = find_game_pid(find);
            if (pid > 0) break;
            if (now_ms() > deadline) {
                say("no wine process matching '%s' found", find);
                return 2;
            }
            usleep(200000);
        }
        say("found target pid %d for '%s'", pid, find);
    }

    int unsupported32 = 0, wow64 = 0;
    if (!target_arch(pid, &unsupported32, &wow64)) {
        say("pid %d is not a supported Wine process%s", pid,
            unsupported32 ? " (old-style 32-bit Wine is not supported; wow64 is required)" : "");
        return unsupported32 ? 4 : 2;
    }
    say("target is %s", wow64 ? "wow64 (32-bit game, 64-bit host side)" : "64-bit");

    if (target_already_applied(pid)) {
        say("thprac signature already present; nothing to do");
        return 0;
    }

    struct user_regs_struct saved;
    memset(&saved, 0, sizeof(saved));
    pid_t tid = 0;
    for (int round = 0; round < 60 && !tid; round++) {
        static struct region scan[4096];
        int n_scan = read_maps(pid, scan, 4096);
        struct range pes[512];
        int n_pes = collect_pe_ranges(scan, n_scan, pes, 512);
        long r = seize_worker(pid, &saved, pes, n_pes);
        if (r == (pid_t)-1) {
            long scope = -1;
            FILE* f = fopen("/proc/sys/kernel/yama/ptrace_scope", "r");
            if (f) { int v = 0; if (fscanf(f, "%d", &v) == 1) scope = v; fclose(f); }
            say("ptrace denied (yama ptrace_scope=%ld). One-time fix:\n"
                "  sudo setcap cap_sys_ptrace=ep %s", scope, argv[0]);
            return 3;
        }
        if (r > 0) tid = r;
        else usleep(100000);
    }
    if (!tid) {
        say("no injectable thread found in pid %d", pid);
        return 2;
    }
    say("seized tid %d", tid);

    int result_code = 7;

    do {
        struct user_regs_struct cur;
        if (get_regs(tid, &cur) < 0) {
            say_errno("GETREGSET");
            break;
        }
        (void)cur;

        /* Wait until the 64-bit PE side is mapped (fresh processes need a moment). */
        static struct region regions[4096];
        int n_regions = 0;
        uint64_t ntdll = 0;
        for (int i = 0; i <= wait_secs * 20; i++) {
            n_regions = read_maps(pid, regions, 4096);
            if (n_regions > 0) {
                ntdll = module_base(regions, n_regions, "/x86_64-windows/ntdll.dll");
                if (ntdll) break;
            }
            if (i == wait_secs * 20) break;
            usleep(50000);
        }
        if (!ntdll) {
            say("kernel32/ntdll not found in maps (loader not ready)");
            result_code = 5;
            break;
        }
        say("ntdll @ %llx", (unsigned long long)ntdll);

        /* We always drive the 64-bit side: LdrLoadDll from the 64-bit ntdll
         * loads our 64-bit loader DLL both into pure 64-bit targets and into
         * wow64 hosts of 32-bit games (the loader then starts the regular
         * 32-bit thprac helper inside the same container). */
        uint64_t ldr_load_dll = pe_export(pid, ntdll, "LdrLoadDll");
        if (!ldr_load_dll) {
            say("LdrLoadDll not found in ntdll exports");
            result_code = 5;
            break;
        }
        say("LdrLoadDll @ %llx", (unsigned long long)ldr_load_dll);

        /* Return trampoline: use an existing int3 in an exec region. */
        uint64_t ret_site = find_int3(pid, regions, n_regions);
        if (!ret_site) {
            say("no int3 found for the return trampoline");
            break;
        }
        say("return trampoline @ %llx", (unsigned long long)ret_site);

        /* Working area on the thread's own stack (below the call frame), well
         * inside its TEB stack limits: the DLL path string. No remote syscalls
         * are made: Wine's seccomp-based syscall interception misbehaves under
         * ptrace, so the only remote call is LoadLibraryW itself. */
        uint64_t teb = saved.gs_base;
        uint64_t stack_base = 0, stack_limit = 0;
        if (!teb || read_mem(pid, teb + 0x08, &stack_base, 8) != 8 ||
            read_mem(pid, teb + 0x10, &stack_limit, 8) != 8 || stack_base <= stack_limit) {
            say("cannot read the thread stack limits from the TEB");
            break;
        }
        say("thread stack [%llx..%llx]", (unsigned long long)stack_limit, (unsigned long long)stack_base);
        uint64_t work = (stack_base - 0x4000) & ~0xfULL; /* string area */
        if (work <= stack_limit + 0x2000) {
            say("thread stack too small");
            break;
        }

        /* Write the DLL path as UTF-16 at work+0x100 and build a
         * UNICODE_STRING at work+0x800, plus flags at work+0x820 and the
         * module handle out-slot at work+0x828. */
        uint16_t wpath[1024];
        size_t wlen = utf8_to_utf16(dll, wpath, 1024);
        if (!write_cstr16(pid, work + 0x100, wpath, wlen)) {
            say_errno("write path");
            break;
        }
        {
            uint8_t us[16] = {};
            uint16_t chars = (uint16_t)(wlen - 1); /* excluding L'\0' */
            us[0] = (uint8_t)(chars * 2);        /* Length */
            us[2] = (uint8_t)((chars + 1) * 2);  /* MaximumLength */
            uint64_t bufptr = work + 0x100;
            memcpy(us + 8, &bufptr, 8);          /* Buffer */
            uint64_t flags = 0, handle = 0;
            if (!write_mem_ok(pid, work + 0x800, us, 16) ||
                !write_mem_ok(pid, work + 0x820, &flags, 8) ||
                !write_mem_ok(pid, work + 0x828, &handle, 8)) {
                say_errno("write UNICODE_STRING");
                break;
            }
        }

        /* Call LdrLoadDll(search_path=NULL, &flags, &us, &handle). */
        if (dry) {
            say("dry run: skipping LdrLoadDll");
            result_code = 0;
            break;
        }
        uint64_t call_result = 0;
        if (hijack_call(tid, ldr_load_dll, 0, work + 0x820, work + 0x800, work + 0x828, NULL, 0, ret_site, &saved, &work, &call_result) < 0) {
            say("LdrLoadDll call failed");
            break;
        }
        if ((int64_t)call_result != 0) {
            say("LdrLoadDll failed: status %08llx (module not loaded; check the path)",
                (unsigned long long)call_result);
            result_code = 6;
            break;
        }
        uint64_t handle = 0;
        read_mem(pid, work + 0x828, &handle, 8);
        say("module loaded at %llx", (unsigned long long)handle);

        /* Optionally mark the target's main exe as thprac-applied (same
         * 'PRAC' signature WriteTHPracSig pokes from the Windows helper).
         * The thread is still stopped at the return trampoline, so a plain
         * PTRACE_POKEDATA works even on the read-only PE header. */
        if (sig) {
            uint64_t peb = 0, image_base = 0, e_lfanew = 0;
            if (read_mem(pid, saved.gs_base + 0x60, &peb, 8) == 8 && peb &&
                read_mem(pid, peb + 0x10, &image_base, 8) == 8 && image_base &&
                read_mem(pid, image_base + 0x3c, &e_lfanew, 4) == 4 && e_lfanew > 4) {
                uint64_t sig_addr = image_base + e_lfanew - 4;
                if (ptrace(PTRACE_POKEDATA, tid, (void*)(uintptr_t)sig_addr,
                           (void*)(uintptr_t)0x43524150UL) == 0) {
                    say("thprac sig written at %llx", (unsigned long long)sig_addr);
                } else {
                    say_errno("sig write (non-fatal)");
                }
            } else {
                say("cannot locate the target image base for --sig (non-fatal)");
            }
        }

        /* Verify the module is visible in maps (basename match). */
        {
            const char* base = strrchr(dll, '\\');
            base = base ? base + 1 : strrchr(dll, '/');
            base = base ? base : dll;
            n_regions = read_maps(pid, regions, 4096);
            int seen = 0;
            for (int i = 0; i < n_regions && !seen; i++)
                if (regions[i].path[0] && strstr(regions[i].path, base)) seen = 1;
            if (!seen) say("note: module not (yet) visible in maps (name mismatch?), continuing");
        }

        result_code = 0;
    } while (0);

    /* Restore the hijacked thread's original registers and detach it. */
    set_regs(tid, &saved);
    ptrace(PTRACE_DETACH, tid, 0, 0);
    say("detached tid %d", tid);

    if (result_code != 0) {
        say("injection failed (code %d)", result_code);
    } else if (spawned) {
        say("test run complete; target keeps running");
    }
    return result_code;
}
