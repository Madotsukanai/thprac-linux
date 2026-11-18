#include "thprac_native.h"
#define NOMINMAX
#include "utils/wininternal.h"

#include "thprac_update.h"
#include "thprac_utils.h"
#include "thprac_identify.h"
#include "thprac_load_exe.h"
#include "thprac_hook.h"
#include "thprac_cfg.h"
#include "thprac_log.h"
#include "thprac_gui_locale.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace THPrac {

constinit wchar_t old_working_dir[MAX_PATH + 1] = {};
extern int Launcher(HINSTANCE hInstance, int nCmdShow);

void RemoteInit() {
    if ((PVOID)(&__ImageBase) == CurrentPeb()->ImageBaseAddress) {
        return;
    }
    if (const auto* ver = IdentifyExe((uint8_t*)CurrentPeb()->ImageBaseAddress, 0, nullptr)) {
        log_init(false, gSettings.console);
        VEHHookInit();
        ver->initFunc();
        ExitThread(0);
    } else {
        FreeLibraryAndExitThread((HMODULE)&__ImageBase, 0);
    }
}

bool PrivilegeCheck() {
    bool ret = false;

    HANDLE hToken;
    if (!NtOpenProcessToken(CurrentProcessHandle, TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION Elevation;
        ULONG cbSize = sizeof(Elevation);
        if (!NtQueryInformationToken(hToken, TokenElevation, &Elevation, cbSize, &cbSize)) {
            ret = Elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return ret;
}

enum RUN_GAME_STATUS {
    RUN_GAME_SUCCESS,
    RUN_GAME_FAILURE,
    RUN_GAME_LAUNCHER,
    RUN_GAME_MALICIOUS,
};

RUN_GAME_STATUS TryRunGame(const wchar_t* exeFn, wchar_t* cmdLine, uint32_t flags, bool prompt, bool warn_unknown_exe) {
    size_t exeLen = t_strlen(exeFn);
    if (_wcsnicmp(exeFn + exeLen - 4, L".exe", 4) != 0) {
        return RUN_GAME_FAILURE;
    }

    ExeInfo exeInfo = {};
    THKnownGame knownGame;
    uint16_t oepCode[10];
    if (!IdentifyKnownGame(knownGame, oepCode, exeFn, &exeInfo)) {
        if (warn_unknown_exe && exeInfo) {
            log_printf("Warning: completely unknown executable %s\r\n", utf16_to_utf8(exeFn).c_str());
            log_printf(" -> timestamp = %d, text size = %d", exeInfo.timeStamp, exeInfo.textSize);
        }
        return RUN_GAME_FAILURE;
    } else if (prompt) {
        int choice = log_mboxf(0, MB_YESNOCANCEL, S(THPRAC_EXISTING_GAME_CONFIRMATION_TITLE), S(THPRAC_EXISTING_GAME_CONFIRMATION), gThGameStrs[knownGame.ver->gameId]);
        switch (choice) {
        case IDYES:
            break;
        case IDNO:
            return RUN_GAME_LAUNCHER;
        default:
            return RUN_GAME_FAILURE;
        }
    }

    if (knownGame.ver->gameId == ID_TH06NC) {
        if (!RunLaunchCommand(L"steam steam://rungameid/4659620")) {
            ShellExecuteW(NULL, L"open", L"steam://rungameid/4659620", nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (!(flags & RUN_FLAG_THPRAC)) {
            return RUN_GAME_SUCCESS;
        }
        for (int i = 0; i < 600; ++i) {
            Sleep(100);
            if (FindAndAttach(false, false, ID_TH06NC)) {
                return RUN_GAME_SUCCESS;
            }
        }
        return RUN_GAME_FAILURE;
    }
    auto* ver = knownGame.ver;
    if (!ver->has_oilp) {
        flags &= ~RUN_FLAG_OILP;
    }
    if (!ver->has_vpatch) {
        flags &= ~RUN_FLAG_VPATCH;
    }
    if (!ver->initFunc) {
        flags &= ~RUN_FLAG_THPRAC;
    }

    switch (knownGame.type) {
    case TYPE_MODDED:
        log_printf("Warning: unknown variant of %s\n", gThGameStrs[knownGame.ver->gameId]);
        log_printf("-> MetroHash: " METRO_HASH_FORMAT "\n", METRO_HASH_ARGS(knownGame.metroHash));
        break;
    case TYPE_MALICIOUS:
        log_printf("Warning: potentially malicious variant of %s\n", gThGameStrs[knownGame.ver->gameId]);
        log_printf("-> MetroHash: " METRO_HASH_FORMAT "\n", METRO_HASH_ARGS(knownGame.metroHash));
        log_printf("-> oepCode: " OEP_CODE_FORMAT "\n", OEP_CODE_ARGS(oepCode));
        if (log_mbox(NULL, MB_ICONWARNING | MB_YESNO, "Potentially malicious game", "The game you are attempting to run is detected to be potentially malicious. Are you sure you want to run it?") != IDYES) {
            return RUN_GAME_MALICIOUS;
        }
        break;
    case TYPE_STEAM:
        ShellExecuteW(NULL, L"open", exeFn, cmdLine, old_working_dir, SW_SHOW);
        for (;;) {
            Sleep(100);
            if (FindAndAttach(false, false, knownGame.ver->gameId)) {
                return RUN_GAME_SUCCESS;
            }
        }
        break;
    default:
        break;
    }

    if (RunGame(exeFn, cmdLine, flags)) {
        return RUN_GAME_SUCCESS;
    }
    else {
        return RUN_GAME_FAILURE;
    }
}
}

wchar_t* NextCommandLineParam(wchar_t* cmdline, wchar_t* out) {
    while (*cmdline == L' ' || *cmdline == L'\t') {
        ++cmdline;
    }

    if (*cmdline == L'\0') {
        return nullptr;
    }

    wchar_t* dst = out;
    bool in_quotes = 0;

    while (*cmdline != L'\0') {
        if (*cmdline == L'\\') {
            unsigned num_backslashes = 0;
            while (*cmdline == L'\\') {
                ++num_backslashes;
                ++cmdline;
            }

            if (*cmdline == L'"') {
                for (unsigned i = 0; i < num_backslashes / 2; ++i) {
                    *dst++ = L'\\';
                }
                if (num_backslashes % 2 == 1) {
                    *dst++ = L'"';
                }
                else {
                    in_quotes = !in_quotes;
                }
                ++cmdline;
            }
            else for (unsigned i = 0; i < num_backslashes; ++i) {
                *dst++ = L'\\';
            }
        }
        else if (*cmdline == L'"') {
            in_quotes = !in_quotes;
            ++cmdline;
        }
        else if (!in_quotes && (*cmdline == L' ' || *cmdline == L'\t')) {
            break;
        }
        else {
            *dst++ = *cmdline++;
        }
    }

    *dst = L'\0';

    while (*cmdline == L' ' || *cmdline == L'\t') {
        ++cmdline;
    }

    return cmdline;
}

using namespace THPrac;

// Steam preloads its game overlay renderer (gameoverlayrenderer*.so) through
// LD_PRELOAD into everything it starts, including this launcher when it is run
// as a non-Steam-game shortcut. The overlay's X11 event hook can crash the
// launcher (0xC0000005 inside gameoverlayrenderer.so, usually via
// XCheckIfEvent from the message pump) as soon as a game window shows up, and
// the launcher itself never needs the overlay. Strip it from the environment
// and run ourselves again without it.
static DWORD StripSteamOverlayAndRelaunch(bool* relaunched) {
    *relaunched = false;
    constexpr wchar_t marker[] = L"THPRAC_OVERLAY_STRIPPED";
    wchar_t markerBuf[4] = {};
    if (GetEnvironmentVariableW(marker, markerBuf, 4)) {
        return 0; // already stripped
    }

    wchar_t preload[4096] = {};
    DWORD len = GetEnvironmentVariableW(L"LD_PRELOAD", preload, 4096);
    if (!len || len >= 4096) {
        return 0;
    }

    wchar_t filtered[4096] = {};
    size_t out = 0;
    bool removed = false;
    for (size_t i = 0; preload[i];) {
        size_t j = i;
        while (preload[j] && preload[j] != L':' && preload[j] != L' ') j++;
        size_t entryLen = j - i;
        if (entryLen) {
            bool overlay = false;
            for (size_t k = 0; k + 19 <= entryLen; k++) {
                if (_wcsnicmp(preload + i + k, L"gameoverlayrenderer", 19) == 0) {
                    overlay = true;
                    break;
                }
            }
            if (overlay) {
                removed = true;
            } else {
                if (out) filtered[out++] = L':';
                wmemcpy(filtered + out, preload + i, entryLen);
                out += entryLen;
            }
        }
        i = j;
        while (preload[i] == L':' || preload[i] == L' ') i++;
    }
    if (!removed) {
        return 0;
    }

    if (out) {
        SetEnvironmentVariableW(L"LD_PRELOAD", filtered);
    } else {
        SetEnvironmentVariableW(L"LD_PRELOAD", nullptr);
    }
    SetEnvironmentVariableW(marker, L"1");

    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, (DWORD)std::size(self));

    STARTUPINFOW si = {
        .cb = sizeof(si),
    };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(self, GetCommandLineW(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return 0; // fall through and run with the overlay still loaded
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    *relaunched = true;
    return code;
}

int WINAPI wWinMain(HINSTANCE hInstance, [[maybe_unused]] HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    // Only strip the Steam overlay when running as the main executable. When
    // this module is loaded into a game (the injector copies of it run with
    // the game image as the main module), GetModuleFileNameW(nullptr)
    // would return the game path and relaunching would start a second game.
    if ((PVOID)&THPrac::__ImageBase == CurrentPeb()->ImageBaseAddress) {
        bool relaunched = false;
        DWORD relaunchCode = StripSteamOverlayAndRelaunch(&relaunched);
        if (relaunched) {
            return (int)relaunchCode;
        }
    }
    InitConfigDir();
    LoadSettings();

    srand(Kuser_Shared_Data->SystemTime.LowPart);

    RemoteInit();
    log_init(true, gSettings.console);

    if (gSettings.thprac_admin_rights && !PrivilegeCheck()) {
        ShellExecuteW(NULL, L"runas", CurrentPeb()->ProcessParameters->ImagePathName.Buffer, pCmdLine, old_working_dir, nCmdShow);
    }
    
    GetCurrentDirectoryW(MAX_PATH, old_working_dir);
    {
        UNICODE_STRING exeDir = CurrentPeb()->ProcessParameters->ImagePathName;
        for (USHORT i = exeDir.Length / sizeof(wchar_t); i > 0; i--) {
            if (exeDir.Buffer[i] == '\\') {
                exeDir.Length = i * sizeof(wchar_t);
                exeDir.MaximumLength = i * sizeof(wchar_t);
                break;
            }
        }
        RtlSetCurrentDirectory_U(&exeDir);
    }

    enum CurrentCmd {
        CMD_NONE,
        CMD_ATTACH,
    };

    CurrentCmd curCmdId = CMD_NONE;
    uint32_t flags = 0xFFFFFFFF;

    wchar_t curCmdStr[512];
    for (wchar_t* p = NextCommandLineParam(GetCommandLineW(), curCmdStr); p; p = NextCommandLineParam(p, curCmdStr)) {
        if (curCmdId == CMD_ATTACH) {
            curCmdId = CMD_NONE;

            int pid = _wtoi(curCmdStr);
            if (ApplyToProcById(pid)) {
                return 0;
            }
            else {
                log_printf("Warning: invalid PID: %d\n", pid);
                continue;
            }
        }
        if (wcscmp(curCmdStr, L"--attach") == 0) {
            curCmdId = CMD_ATTACH;
            continue;
        }
        if (wcscmp(curCmdStr, L"--without-vpatch") == 0) {
            flags &= ~RUN_FLAG_VPATCH;
            continue;
        }
        if (wcscmp(curCmdStr, L"--without-oilp") == 0) {
            flags &= ~RUN_FLAG_OILP;
            continue;
        }

        if (PathsCompare(curCmdStr, t_strlen(curCmdStr), CurrentPeb()->ProcessParameters->ImagePathName.Buffer, CurrentPeb()->ProcessParameters->ImagePathName.Length / 2)) {
            continue;
        }

        switch (TryRunGame(curCmdStr, p, flags, false, true)) {
        case RUN_GAME_SUCCESS:
            return 0;
        case RUN_GAME_MALICIOUS:
            return 1;
        default:
            continue;
        }
    }

    if (curCmdId == CMD_ATTACH) {
        // Exit code tells the cross-prefix helper (thprac_wine_attach.cpp) whether a game was handled.
        // THPRAC_ATTACH_WAIT=<seconds> keeps polling for a game to appear instead
        // of giving up at once; the cross-container dispatch starts this helper
        // before the game has finished launching.
        wchar_t waitBuf[16] = {};
        int waitSecs = 0;
        if (GetEnvironmentVariableW(L"THPRAC_ATTACH_WAIT", waitBuf, 16) > 0) {
            waitSecs = _wtoi(waitBuf);
        }
        if (waitSecs > 0) {
            for (int i = 0; i < waitSecs * 5; ++i) {
                if (FindAndAttach(false, false)) {
                    return 0;
                }
                Sleep(200);
            }
            return 1;
        }
        return FindAndAttach(false, false) ? 0 : 1;
    }
    else if (!gSettings.dont_search_ongoing_game && FindAndAttach(false, true)) {
        return 0;
    }

    // Look for a known game executable in the same directory as thprac.exe
    // (and, failing that, in the directory thprac was started from).
    // If one is found, it is launched with thprac hooked into it automatically.
    if (gSettings.existing_game_launch_action != LAUNCH_ACTION_OPEN_LAUNCHER) {
        std::wstring searchDirs[2];
        {
            // The current directory was set to thprac's own directory above.
            wchar_t cur[MAX_PATH + 1] = {};
            GetCurrentDirectoryW(MAX_PATH, cur);
            searchDirs[0] = cur;
        }
        searchDirs[1] = old_working_dir;

        const wchar_t* selfPath = CurrentPeb()->ProcessParameters->ImagePathName.Buffer;
        const size_t selfPathLen = CurrentPeb()->ProcessParameters->ImagePathName.Length / sizeof(wchar_t);

        bool openLauncher = false;
        for (int dirIdx = 0; dirIdx < 2 && !openLauncher; dirIdx++) {
            std::wstring& dir = searchDirs[dirIdx];
            while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
                dir.pop_back();
            }
            if (dir.empty()) {
                continue;
            }
            if (dirIdx == 1 && PathsCompare(searchDirs[0].c_str(), searchDirs[0].size(), dir.c_str(), dir.size())) {
                continue;
            }

            for (int originalNamePass = 1; originalNamePass >= 0 && !openLauncher; originalNamePass--) {
                WIN32_FIND_DATAW find = {};
                std::wstring pattern = dir + L"\\*.exe";
                HANDLE hFind = FindFirstFileW(pattern.c_str(), &find);
                if (hFind == INVALID_HANDLE_VALUE) {
                    break;
                }
                do {
                    if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        continue;
                    }

                    THGameID idFromExeName = ParseExeName(COUNTED(find.cFileName));

                    // fucked up ternary
                    if (originalNamePass ? idFromExeName == ID_UNKNOWN : idFromExeName != ID_UNKNOWN) {
                        continue;
                    }

                    // Always pass the full path so the game gets its own directory as the working directory.
                    std::wstring fullPath = dir + L"\\" + find.cFileName;
                    if (PathsCompare(fullPath.c_str(), fullPath.size(), selfPath, selfPathLen)) {
                        continue;
                    }

                    const RUN_GAME_STATUS status = TryRunGame(fullPath.c_str(), nullptr, flags, gSettings.existing_game_launch_action == LAUNCH_ACTION_ALWAYS_ASK, false);
                    if (status == RUN_GAME_SUCCESS) {
                        FindClose(hFind);
                        return 0;
                    }
                    if (status == RUN_GAME_LAUNCHER) {
                        openLauncher = true;
                        break;
                    }
                } while (FindNextFileW(hFind, &find));
                FindClose(hFind);
            }
        }
    }

    return Launcher(hInstance, nCmdShow);
}
