#define NOMINMAX

#include "thprac_load_exe.h"
#include "thprac_identify.h"
#include "thprac_attach_log.h"
#include "thprac_log.h"
#include "thprac_gui_locale.h"
#include "thprac_native.h"
#include "thprac_wine_attach.h"
#include "thprac_utils.h"
#include "thprac_th06nc.h"
#include "utils/utils.h"
#include "utils/wininternal.h"

#include <tlhelp32.h>
#include <wchar.h>

#include <algorithm>
#include <string>

namespace THPrac {

// -----------------------------------------------------------------------
// Native injector runner. ALL remote module loading (thprac itself, vpatch,
// oilp, the th06nc practice module) goes through the embedded ptrace
// injector; no Windows-side code touches other processes directly anymore.
// -----------------------------------------------------------------------
static bool ReadSmallFile(const wchar_t* path, std::string& out) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[256] = {};
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    out.assign(buf, got);
    return true;
}

// dllPath: module to load (DOS path). target: Linux pid (pidIsPid) or a
// comma-separated list of exe basenames to discover. markSig: write the
// 'PRAC' applied marker. The injector waits for the target to appear, so
// freshly launched games are covered.
static bool RunInjector(const wchar_t* dllPath, const char* target, bool pidIsPid, bool markSig, int waitSecs) {
    std::wstring bundleDir;
    if (!NativeBundle::Present() || !NativeBundle::Prepare(bundleDir)) {
        AttachLog("injector: bundle missing");
        return false;
    }
    wchar_t dir[32768] = {};
    lstrcpynW(dir, bundleDir.c_str(), 32000);
    wchar_t* sep = dir + lstrlenW(dir);
    while (sep > dir && *sep != L'\\' && *sep != L'/') --sep;
    *sep = 0;
    std::wstring injectorPath = std::wstring(dir) + L"\\thprac-injector";

    wchar_t tmp[MAX_PATH + 1] = {};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n > MAX_PATH) return false;
    std::wstring resultW = std::wstring(tmp, n) + L"thprac_inj_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount()) + L".res";
    DeleteFileW(resultW.c_str());

    std::wstring args = std::wstring(pidIsPid ? L"--pid " : L"--find ")
        + utf8_to_utf16(target)
        + L" --dll \"" + dllPath + L"\""
        + (markSig ? L" --sig" : L"")
        + L" --wait " + std::to_wstring(waitSecs)
        + L" --result \"" + resultW + L"\"";
    std::wstring cmd = injectorPath + L" " + args;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    wchar_t cmdMut[4096];
    lstrcpynW(cmdMut, cmd.c_str(), 4095);
    if (!CreateProcessW(injectorPath.c_str(), cmdMut, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        AttachLog("injector: CreateProcessW failed err=%lu", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD startTick = GetTickCount();
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0) break;
        if (GetTickCount() - startTick > (DWORD)(waitSecs * 4 + 10) * 1000) break;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    std::string res;
    ReadSmallFile(resultW.c_str(), res);
    DeleteFileW(resultW.c_str());
    AttachLog("injector: exit=%lu result=%.160s", code, res.c_str());
    return code == 0;
}

// Inject the full thprac module (this executable) into a game.
static bool InjectSelfViaInjector(const char* linuxPidOrFind, bool pidIsPid, int waitSecs) {
    wchar_t self[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, self, (DWORD)std::size(self))) return false;
    return RunInjector(self, linuxPidOrFind, pidIsPid, true, waitSecs);
}

// Inject the th06nc practice module (and its freetype dependency) from the
// payload bundle.
static bool InjectTh06ncViaInjector(const char* linuxPidOrFind, bool pidIsPid, const LaunchOptions& opts, int waitSecs) {
    std::wstring bundleDir;
    if (!NativeBundle::Present() || !NativeBundle::Prepare(bundleDir)) {
        AttachLog("injector(nc): bundle missing");
        return false;
    }
    WriteLaunchOptionsFile(opts);
    wchar_t dir[32768] = {};
    lstrcpynW(dir, bundleDir.c_str(), 32000);
    wchar_t* sep = dir + lstrlenW(dir);
    while (sep > dir && *sep != L'\\' && *sep != L'/') --sep;
    *sep = 0;
    return RunInjector((std::wstring(dir) + L"\\thprac_th06nc.dll").c_str(), linuxPidOrFind, pidIsPid, true, waitSecs);
}

bool CheckDLLFunction(const wchar_t* path, const char* funcName) {
    MappedFile file(path);

    auto exeSize = file.fileSize;
    auto exeBuffer = file.fileMapView;
    if (exeSize < 128)
        return 0;
    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)exeBuffer;
    if (!pDosHeader || pDosHeader->e_magic != 0x5a4d || (size_t)pDosHeader->e_lfanew + 512 >= exeSize)
        return 0;
    PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((DWORD)exeBuffer + pDosHeader->e_lfanew);
    if (!pNtHeader || pNtHeader->Signature != 0x00004550)
        return 0;
    PIMAGE_SECTION_HEADER pSection = IMAGE_FIRST_SECTION(pNtHeader);
    if (!pSection)
        return 0;

    if (pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress != 0 && pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size != 0) {
        auto pExportSectionVA = pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        for (DWORD i = 0; i < pNtHeader->FileHeader.NumberOfSections; i++, pSection++) {
            if (pSection->VirtualAddress <= pExportSectionVA && pSection->VirtualAddress + pSection->SizeOfRawData > pExportSectionVA) {
                auto pSectionBase = (DWORD)exeBuffer - pSection->VirtualAddress + pSection->PointerToRawData;
                PIMAGE_EXPORT_DIRECTORY pExportDirectory = (PIMAGE_EXPORT_DIRECTORY)(pSectionBase + pExportSectionVA);
                char** pExportNames = (char**)(pSectionBase + pExportDirectory->AddressOfNames);
                for (DWORD j = 0; j < pExportDirectory->NumberOfNames; ++j) {
                    auto pFunctionName = (char*)(pSectionBase + pExportNames[j]);
                    if (!strcmp(pFunctionName, funcName)) {
                        return true;
                    }
                }
            }
        }
    }

    return true;
}

PIMAGE_NT_HEADERS GetNtHeader(HMODULE hMod)
{
    if (!hMod) {
        return 0;
    }
    PIMAGE_DOS_HEADER pDosH = (PIMAGE_DOS_HEADER)hMod;
    PIMAGE_NT_HEADERS pNTH = (PIMAGE_NT_HEADERS)((UINT_PTR)pDosH + pDosH->e_lfanew);
    return pNTH;
}

void* GetNtDataDirectory(HMODULE hMod, BYTE directory)
{
    if (PIMAGE_NT_HEADERS pNTH = GetNtHeader(hMod)) {
        if (UINT_PTR DirVA = pNTH->OptionalHeader.DataDirectory[directory].VirtualAddress) {
            return (BYTE*)hMod + DirVA;
        }
    }
    return NULL;
}


const wchar_t* allMutexNames[] = {
    L"Touhou Koumakyou App",
    L"Touhou YouYouMu App",
    L"Touhou 08 App",
    L"Touhou 10 App",
    L"Touhou 11 App",
    L"Touhou 12 App",
    L"th17 App",
    L"th18 App",
    L"th185 App",
    L"th19 App",
    L"th20 App",
};

bool CheckIfAnyGame() {
    for (const wchar_t* mutexName : allMutexNames) {
        HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, mutexName);
        if (hMutex) {
            CloseHandle(hMutex);
            return true;
        }
    }
    return false;
}

// Identify the game from the target's /proc/<pid>/maps via the Z: drive
// (plain filesystem access) so we know which module to inject. The caller
// passes a Linux pid (it comes from the Unix-side scripts/helpers).
static THGameID GameIdByLinuxPid(DWORD linuxPid) {
    wchar_t path[64];
    wsprintfW(path, L"Z:\\proc\\%lu\\maps", linuxPid);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return ID_UNKNOWN;
    char buf[65536] = {};
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    if (!got) return ID_UNKNOWN;
    // First file-backed .exe mapping = the game image.
    char* line = buf;
    char* end = buf + got;
    while (line < end) {
        char* nl = (char*)memchr(line, '\n', end - line);
        size_t len = nl ? (size_t)(nl - line) : (size_t)(end - line);
        char* lastSpace = nullptr;
        for (size_t i = len; i > 0; --i) {
            if (line[i - 1] == ' ') { lastSpace = line + i; break; }
        }
        if (lastSpace && lastSpace < line + len) {
            std::wstring name = utf8_to_utf16(lastSpace);
            size_t slash = name.find_last_of(L"/\\");
            std::wstring base = slash == std::wstring::npos ? name : name.substr(slash + 1);
            if (!base.empty()) {
                THGameID id = ParseExeName(base.c_str(), base.size());
                if (id != ID_UNKNOWN) return id;
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    return ID_UNKNOWN;
}

// Canonical exe basename for the injector's --find discovery.
std::string PrimaryExeNameForGame(THGameID id) {
    switch (id) {
    case ID_TH06NC: return "th06nc.exe";
    case ID_ALCOSTG: return "algostg.exe";
    case ID_TH06: return "th06.exe";
    case ID_TH07: return "th07.exe";
    case ID_TH075: return "th075.exe";
    case ID_TH08: return "th08.exe";
    case ID_TH09: return "th09.exe";
    case ID_TH095: return "th095.exe";
    case ID_TH10: return "th10.exe";
    case ID_TH105: return "th105.exe";
    case ID_TH11: return "th11.exe";
    case ID_TH12: return "th12.exe";
    case ID_TH123: return "th123.exe";
    case ID_TH125: return "th125.exe";
    case ID_TH128: return "th128.exe";
    case ID_TH13: return "th13.exe";
    case ID_TH135: return "th135.exe";
    case ID_TH14: return "th14.exe";
    case ID_TH143: return "th143.exe";
    case ID_TH145: return "th145.exe";
    case ID_TH15: return "th15.exe";
    case ID_TH155: return "th155.exe";
    case ID_TH16: return "th16.exe";
    case ID_TH165: return "th165.exe";
    case ID_TH17: return "th17.exe";
    case ID_TH175: return "th175.exe";
    case ID_TH18: return "th18.exe";
    case ID_TH185: return "th185.exe";
    case ID_TH19: return "th19.exe";
    case ID_TH20: return "th20.exe";
    default: return {};
    }
}

bool ApplyToProcById(DWORD pid) {
    THGameID id = GameIdByLinuxPid(pid);
    AttachLog("ApplyToProcById pid=%lu id=%d", (unsigned long)pid, (int)id);
    if (id == ID_TH06NC) {
        LaunchOptions opts{};
        opts.practice = 1;
        opts.language = Gui::LocaleGet();
        return InjectTh06ncViaInjector(std::to_string((unsigned long long)pid).c_str(), true, opts, 20);
    }
    if (id == ID_UNKNOWN) {
        // Cannot read the target (other container?): fall back to scanning.
        return FindAndAttach(false, false);
    }
    return InjectSelfViaInjector(std::to_string((unsigned long long)pid).c_str(), true, 20);
}

bool FindAndAttach(bool prompt_if_no_game, bool prompt_if_yes_game, THGameID gameID) {
    AttachLog("FindAndAttach(no=%d yes=%d game=%d)", prompt_if_no_game?1:0, prompt_if_yes_game?1:0, (int)gameID);
    bool hasPrompted = false;
    // Identification is by exe image name only (no cross-process access);
    // version checking happens inside the game once the module is loaded.
    auto HasThpracSupport = [](THGameID id) -> bool {
        if (id == ID_TH06NC) return true;
        for (unsigned int i = 0; i < gGameVersionsCount; i++)
            if (gGameVersions[i].gameId == id && gGameVersions[i].initFunc) return true;
        return false;
    };
    auto TryProcess = [&](SYSTEM_PROCESS_INFORMATION* proc) -> bool {
        THGameID exeGameId = ParseExeName(UNICODE_STRING_PARAM(proc->ImageName));
        if (gameID == ID_UNKNOWN ? exeGameId == ID_UNKNOWN : exeGameId != gameID) {
            return false;
        }
        if (!HasThpracSupport(exeGameId)) {
            return false;
        }
        AttachLog("found pid=%lu game=%d nc=%d prompt=%d", (unsigned long)proc->UniqueProcessId, (int)exeGameId,
            exeGameId == ID_TH06NC, prompt_if_yes_game ? 1 : 0);
        if (prompt_if_yes_game) {
            hasPrompted = true;
            const char* title = exeGameId == ID_TH06NC ? S(TH06NC_TITLE) : gThGameStrs[exeGameId];
            int choice = log_mboxf(0, MB_YESNO, S(THPRAC_PR_APPLY), S(THPRAC_PR_ASK_ATTACH), title);
            if (choice != IDYES) {
                AttachLog("pid=%lu user declined", (unsigned long)proc->UniqueProcessId);
                return false;
            }
        }
        bool applied;
        if (exeGameId == ID_TH06NC) {
            LaunchOptions opts{};
            opts.practice = 1;
            opts.language = Gui::LocaleGet();
            applied = InjectTh06ncViaInjector("th06nc.exe", false, opts, 20);
        } else {
            std::wstring imageName(UNICODE_STRING_PARAM(proc->ImageName));
            size_t slash = imageName.find_last_of(L"\\/");
            std::wstring base = slash == std::wstring::npos ? imageName : imageName.substr(slash + 1);
            applied = InjectSelfViaInjector(utf16_to_utf8(base.c_str()).c_str(), false, 20);
        }
        AttachLog("pid=%lu applied=%d", (unsigned long)proc->UniqueProcessId, applied ? 1 : 0);
        if (applied) {
            if (prompt_if_yes_game) {
                log_mbox(0, MB_ICONASTERISK | MB_OK, S(THPRAC_PR_COMPLETE), S(THPRAC_PR_INFO_ATTACHED));
            }
            return true;
        } else {
            log_mbox(0, MB_ICONERROR | MB_OK, S(THPRAC_PR_ERROR), S(THPRAC_PR_ERR_ATTACH));
            return true;
        }
    };

    // NC does not have to expose one of the original games' named mutexes.
    if (gameID == ID_UNKNOWN || gameID == ID_TH06NC || CheckIfAnyGame()) {
        ULONG bufLen = 0;
        NtQuerySystemInformation(SystemProcessInformation, nullptr, 0, &bufLen);
        LPVOID buf = VirtualAlloc(nullptr, bufLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) {
            goto no_game;
        }
        defer (if (buf) VirtualFree(buf, 0, MEM_RELEASE));
        if (!NT_SUCCESS(NtQuerySystemInformation(SystemProcessInformation, buf, bufLen, &bufLen))) {
            goto no_game;
        }

        for (auto* proc = (SYSTEM_PROCESS_INFORMATION*)buf; proc;
             proc = proc->NextEntryOffset ? (SYSTEM_PROCESS_INFORMATION*)((uintptr_t)proc + proc->NextEntryOffset) : nullptr
        ) {
            if (TryProcess(proc)) {
                return true;
            }
        }
    }

no_game:
    // Games in other Wine prefixes (other Proton instances, plain Wine, Lutris,
    // Bottles) are invisible to this wineserver; hand them to the Unix-side helper.
    if (WineAttach::IsWine()) {
        bool wineAsked = false;
        if (WineAttach::AttachOtherPrefixes(gameID, prompt_if_no_game, prompt_if_yes_game, &wineAsked)) {
            return true;
        }
        if (wineAsked) {
            hasPrompted = true;
        }
    }
    if (prompt_if_no_game && !hasPrompted) {
        AttachLog("no game found/handled");
        log_mbox(0, MB_ICONERROR | MB_OK, S(THPRAC_PR_ERROR), S(THPRAC_PR_ERR_NO_GAME));
    }

    return false;
}

bool RunGame(const wchar_t* exeFn, wchar_t* cmdLine, uint32_t flags) {
    STARTUPINFOW si = {
        .cb = sizeof(si),
    };
    PROCESS_INFORMATION pi = {};

    const wchar_t* file_spec = std::max(wcsrchr(exeFn, L'\\'), wcsrchr(exeFn, L'/'));
    std::wstring exeDir;
    BOOL ret;
    if (file_spec) {
        exeDir = std::wstring(exeFn, file_spec);
        ret = CreateProcessW(exeFn, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, exeDir.c_str(), &si, &pi);
    } else {
        ret = CreateProcessW(exeFn, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    }

    if (!ret) {
        log_printf("thprac: failed to start %s (error %lu)\n", utf16_to_utf8(exeFn).c_str(), GetLastError());
        return false;
    }

    // The game is started unsuspended; the native injector discovers its
    // Linux pid by exe name and loads the requested modules through ptrace
    // once the Wine loader is ready.
    std::string baseName = utf16_to_utf8(file_spec ? file_spec + 1 : exeFn);
    bool res = false;
    if (flags & RUN_FLAG_OILP) {
        std::wstring pos = exeDir + L"\\openinputlagpatch.dll";
        res = RunInjector(pos.c_str(), baseName.c_str(), false, false, 20);
    }
    if (!res && (flags & RUN_FLAG_VPATCH)) {
        // Pick the vpatch dll locally (export check happens in our own
        // process); the remote load itself goes through the injector.
        std::wstring curdir_bak;
        {
            auto& c = CurrentPeb()->ProcessParameters->CurrentDirectory.DosPath;
            curdir_bak.assign(c.Buffer, c.Length / sizeof(wchar_t));
            SetCurrentDirectoryW(exeDir.c_str());
        }
        WIN32_FIND_DATAW find;
        HANDLE hFind = FindFirstFileW(L"vpatch*.dll", &find);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (CheckDLLFunction(find.cFileName, "_Initialize@4")) {
                    RunInjector((exeDir + L"\\" + find.cFileName).c_str(), baseName.c_str(), false, false, 20);
                    break;
                }
            } while (FindNextFileW(hFind, &find));
            FindClose(hFind);
        }
        SetCurrentDirectoryW(curdir_bak.c_str());
    }

    if (flags & RUN_FLAG_THPRAC) {
        InjectSelfViaInjector(baseName.c_str(), false, 30);
    }

    // TODO: determine if these should be returned
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return true;
}
}
