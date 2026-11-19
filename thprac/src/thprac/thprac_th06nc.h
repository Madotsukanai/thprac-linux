#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include "thprac_log_dir.h"
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <dxgi.h>

#if defined(_MSC_VER)
#define TH_NOINLINE __declspec(noinline)
#else
#define TH_NOINLINE __attribute__((noinline))
#endif

#if defined(__MINGW32__) || defined(__GNUC__)
#define _TRUNCATE ((size_t)-1)
inline errno_t wcsncpy_s(wchar_t* dest, size_t destsz, const wchar_t* src, size_t count) {
    if (!dest || !destsz) return 22;
    if (count == _TRUNCATE) { wcsncpy(dest, src, destsz - 1); dest[destsz - 1] = 0; return 0; }
    wcsncpy(dest, src, count < destsz ? count : destsz - 1); dest[destsz - 1] = 0; return 0;
}
#ifndef __MINGW32__
inline size_t strnlen_s(const char* s, size_t maxsize) { if(!s) return 0; size_t n=0; while(n<maxsize && s[n]) n++; return n; }
#endif
#define _stricmp strcasecmp
#endif

namespace THPrac::TH06NC {
// Append NC-only IDs so existing TH06 selections and saved replays stay stable.
inline constexpr int FragileWing=71, CreepingBloodstain=72, DanmakuHeart=73;
inline constexpr std::array<int,7> PortionCounts={6,4,7,9,5,2,7};
inline constexpr bool IsAddedSpell(int section) {
    return section>=FragileWing && section<=DanmakuHeart;
}
}

constexpr uint32_t Magic = 0x4e435031, Protocol = 10, ReplayProtocol = 7;
constexpr wchar_t ExpectedHash[] = L"07850c8c6e469c0e82c13423e6d0d096a88d693455bdacacbb44c0aa3bcce473";
constexpr wchar_t Steam25306795Hash[] = L"48630a42a2eb6762d0db2a7e0d151203efbe67220fef7d2f9d0d51857928ac82";
enum class GameBuild { Unsupported, Original, Steam25306795 };
enum Flags : uint32_t { Invincible=1, InfiniteLives=2, InfiniteBombs=4, InfinitePower=8,
    TimeLock=16, AutoBomb=32, CustomRank=64, KeepBgm=128 };
struct Settings {
    int enabled=1, stage=1, difficulty=1, shot=0, spell=-1, frame=0;
    int lives=8, bombs=8, power=128, graze=0, point=0, rank=32;
    uint64_t score=0;
    uint32_t flags=0, restart=0;
    int fakeShot=-1, section=0, phase=0, dialogue=0;
    int fps=60;
    uint32_t reserved=0;
};
struct Status {
    int ready=0, error=0, stage=0, difficulty=0, frame=0, lives=0, bombs=0, power=0, rank=0;
    uint32_t starts=0;
    uint64_t score=0;
    uint32_t flags=0, exports=0;
    int misses=0, bombsUsed=0;
    wchar_t message[160]{};
};
// UI language follows thprac's locale order: Chinese, English, Japanese.
// Keep it outside Settings so it cannot affect practice or replay parameters.
struct Shared {
    uint32_t magic=Magic, version=Protocol;
    Settings settings{}; Status status{};
    int language=1, practiceEnabled=1, lowLatencyState=0;
    uint32_t lowLatencyError=0;
};
// Supplied by the bridge before injection; immutable for this game process.
struct LaunchOptions { uint32_t magic=Magic, version=Protocol; int practice=1, lowLatency=0, language=1, build=0; };
inline std::wstring mapName(DWORD pid) { return L"Global\\Th06NcPractice_"+std::to_wstring(pid); }
inline std::wstring mutexName(DWORD pid) { return mapName(pid)+L"_lock"; }
inline std::wstring launchName(DWORD pid) { return mapName(pid)+L"_launch"; }

// Options file handed over through the filesystem ($HOME is shared between
// containers), used when the in-game module is loaded by the native injector
// and no wineserver named object could be prepared beforehand.
inline std::wstring launchOptionsPath() {
    wchar_t home[1024] = {};
    std::wstring p = L"Z:\\home";
    if (GetEnvironmentVariableW(L"HOME", home, 1024) && home[0] == L'/') {
        p = std::wstring(L"Z:") + home;
    }
    for (auto& c : p) if (c == L'/') c = L'\\';
    return p + L"\\.cache\\thprac-wine\\th06nc_launch.opt";
}
inline bool WriteLaunchOptionsFile(const LaunchOptions& o) {
    HANDLE f = CreateFileW(launchOptionsPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(f, &o, sizeof(o), &wrote, nullptr) && wrote == sizeof(o);
    CloseHandle(f);
    return ok;
}
// Reads and deletes the file so stale options never leak into later runs.
inline bool ReadLaunchOptionsFile(LaunchOptions& o) {
    HANDLE f = CreateFileW(launchOptionsPath().c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LaunchOptions tmp{};
    DWORD got = 0;
    bool ok = ReadFile(f, &tmp, sizeof(tmp), &got, nullptr) && got == sizeof(tmp) && tmp.magic == Magic && tmp.version == Protocol;
    CloseHandle(f);
    DeleteFileW(launchOptionsPath().c_str());
    if (ok) o = tmp;
    return ok;
}
inline std::wstring executablePath(HMODULE module=nullptr) {
    std::wstring s(32768,L'\0'); auto n=GetModuleFileNameW(module,s.data(),DWORD(s.size())); s.resize(n); return s;
}
inline std::wstring directory(const std::wstring& p) { return p.substr(0,p.find_last_of(L"\\/")); }
constexpr wchar_t DiagLogName[] = L"thprac-linux.log";
inline bool DiagEnabled() {
    static bool on = [] { wchar_t v[8]; return GetEnvironmentVariableW(L"TH06NC_TRACE", v, 8) > 0; }();
    return on;
}
inline void DiagWriteLine(const wchar_t* path, const char* text) {
    if (!DiagEnabled()) return;
    FILE* f = _wfopen(path, L"ab");
    if (f) { fputs("[dll] ", f); fputs(text, f); fputc('\n', f); fclose(f); }
}
inline void DiagLog(const char* msg) {
    std::wstring path = THPrac::LogPath(DiagLogName);
    DiagWriteLine(path.empty() ? DiagLogName : path.c_str(), msg);
}
inline void DiagLogf(const char* fmt, ...) {
    char line[512];
    va_list args; va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    DiagLog(line);
}
inline GameBuild identifyBuild(const std::wstring& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file==INVALID_HANDLE_VALUE) {DiagLogf("identify open fail err=%lu",GetLastError()); return GameBuild::Unsupported;}
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{}; GameBuild build=GameBuild::Unsupported;
    NTSTATUS st=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0);
    if(st>=0) st=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
    if(algorithm && hash) {
        std::vector<unsigned char> buffer(65536); DWORD n=0; bool readOk=true;
        while(true) { if(!ReadFile(file,buffer.data(),DWORD(buffer.size()),&n,nullptr)) {readOk=false;DiagLogf("identify read fail err=%lu",GetLastError());break;}
          if(!n) break; st=BCryptHashData(hash,buffer.data(),n,0); if(st<0){readOk=false;DiagLogf("identify hash fail st=%ld",(long)st);break;}
        }
        unsigned char digest[32]{}; if(readOk) st=BCryptFinishHash(hash,digest,32,0); else st=-1;
        if(readOk && st>=0) {
          std::wstring value; const wchar_t* hex=L"0123456789abcdef";
          for(auto c:digest){value+=hex[c>>4];value+=hex[c&15];}
          if(value==ExpectedHash)build=GameBuild::Original;
          else if(value==Steam25306795Hash)build=GameBuild::Steam25306795;
        }
    }
    if(hash)BCryptDestroyHash(hash); if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0); CloseHandle(file);
    DiagLogf("identify done build=%u",(unsigned)build);
    return build;
}
inline bool supportedFile(const std::wstring& path) { return identifyBuild(path)!=GameBuild::Unsupported; }
inline bool valid(const Settings& s) {
    constexpr int lastSections[]={6,12,21,30,38,49,73};
    if(s.stage<1||s.stage>7)return false;
    bool portion=s.section/100==100+s.stage && s.section%100>=1 && s.section%100<=THPrac::TH06NC::PortionCounts[s.stage-1];
    bool boss=s.section>=(s.stage==1?1:lastSections[s.stage-2]+1) && s.section<=lastSections[s.stage-1];
    if(s.section!=0&&!portion&&!boss)return false;
    return s.stage>=1&&s.stage<=7 && s.difficulty>=0&&s.difficulty<=4 && s.shot>=0&&s.shot<=3 &&
      s.spell>=-1&&s.spell<134 && s.frame>=0&&s.frame<=32766 && s.lives>=0&&s.lives<=8 &&
      s.bombs>=0&&s.bombs<=8 && s.power>=0&&s.power<=128 && s.graze>=0&&s.graze<=99999 &&
      s.point>=0&&s.point<=65535 && s.rank>=0&&s.rank<=99 && s.score<=9999999990ULL &&
      s.fakeShot>=-1&&s.fakeShot<=3 && s.fps>=30&&s.fps<=240 && s.phase>=0&&s.phase<=1 &&
      (s.flags&~255u)==0 && (s.dialogue==0||s.dialogue==1) && (s.enabled==0||s.enabled==1);
}

namespace THPrac::TH06NC {
using PresentCallback = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
// Display state is separate from Settings and is never written into replays.
enum class DisplayState : int { Off, Pending, Active, Unavailable };
bool InstallLowLatency(void* resizeBuffersEntry);
void LowLatencyBeforeFrame();
HRESULT LowLatencyPresent(IDXGISwapChain* swap, UINT interval, UINT flags, PresentCallback original);
DisplayState LowLatencyState();
uint32_t LowLatencyError();
}
