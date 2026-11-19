#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>

#include "thprac_identify.h"

namespace THPrac
{

PIMAGE_NT_HEADERS GetNtHeader(HMODULE hMod);
void* GetNtDataDirectory(HMODULE hMod, BYTE directory);



// DO NOT CHANGE THIS STRUCT WITHOUT RECOMPILING init_shellcode.cpp
struct alignas(16) remote_param {
    UINT_PTR pRemoteParamAddr;
    decltype(LoadLibraryW)* pLoadLibraryW;
    decltype(LoadLibraryA)* pLoadLibraryA;
    decltype(VirtualProtect)* pVirtualProtect;
    decltype(GetProcAddress)* pGetProcAddress;
    wchar_t sExePath[MAX_PATH];
    char sLoadErrDllName[MAX_PATH];
};

enum RunFlags {
    RUN_FLAG_THPRAC = 1 << 0,
    RUN_FLAG_SKIP_IDENTIFY = 1 << 1,
    RUN_FLAG_ALWAYS = 1 << 2,
    RUN_FLAG_OILP = 1 << 3,
    RUN_FLAG_VPATCH = 1 << 4,
};

uintptr_t GetProcessModuleBase(HANDLE hProc);
const THGameVersion* CheckOngoingGameByPID(DWORD pid, uintptr_t* base, HANDLE* pOutHandle);
bool CheckIfAnyGame();
const wchar_t* GetGameMutexName(THGameID game);
bool CheckTHPracSig(HANDLE hProc, uintptr_t base);
bool WriteTHPracSig(HANDLE hProc, uintptr_t base);
bool LoadSelf(HANDLE hProcess);
uintptr_t LoadRemoteLibrary(HANDLE hProcess, const wchar_t* libName);
bool TryLoadVpatch(HANDLE hProcess, const wchar_t* exeDir);
bool FindAndAttach(bool prompt_if_no_game, bool prompt_if_yes_game, THGameID id = ID_UNKNOWN,
    const char* steamAppId = nullptr, uint32_t patchFlags = 0, DWORD* outPid = nullptr,
    const wchar_t* requiredExeName = nullptr);
std::string PrimaryExeNameForGame(THGameID id);
// Resolve the actual game executable name from a Steam launch command
// ("steam steam://rungameid/<appid>") by reading the Steam library metadata.
// Returns e.g. "東方紅魔郷.exe" or "th06nc.exe"; empty when not resolvable.
std::string SteamAppExeName(const std::string& command);
bool ApplyToProcById(DWORD pid);
bool RunGame(const wchar_t* exeFn, wchar_t* cmdLine, uint32_t flags = 0xFFFFFFFF);
bool CheckDLLFunction(const wchar_t* path, const char* funcName);

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

}
