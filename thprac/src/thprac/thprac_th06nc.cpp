#include "thprac_th06nc.h"
#ifndef THPRAC_TH06NC_BRIDGE
#include "thprac_th06nc_map.h"
#endif

#ifdef THPRAC_TH06NC_BRIDGE
// Bridge process launcher (injected, extracts payloads and starts th06nc).

#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <fstream>

#define IDR_FREETYPE_DLL 101
#define IDR_MODULE_DLL   102

namespace {
Shared* shared{}; HANDLE mapping{},mutex{}; DWORD targetPid{};
void log(const std::wstring& text) {
    if(!DiagEnabled())return;
    static std::ofstream* f=nullptr;
    if(!f){ static std::ofstream fs; std::wstring p=directory(executablePath())+L"\\"+DiagLogName; fs.open(std::string(p.begin(),p.end()), std::ios::app); f=&fs; }
    if(f&&f->is_open()){
        int n=WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,nullptr,0,nullptr,nullptr);
        std::string u(n>1?n-1:0,'\0'); if(n>1) WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,u.data(),n,nullptr,nullptr);
        (*f)<<u<<"\n"; f->flush();
    }
}
void failure(const std::wstring& text) { log(L"FAIL: "+text); MessageBoxW(nullptr,text.c_str(),L"thprac-linux",MB_ICONERROR); }
void disconnect() { if(shared)UnmapViewOfFile(shared);if(mapping)CloseHandle(mapping);if(mutex)CloseHandle(mutex);shared=nullptr;mapping=nullptr;mutex=nullptr;targetPid=0; }
bool writeResource(int id,const std::wstring& path) {
    HRSRC res=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA); if(!res) return false;
    HGLOBAL data=LoadResource(nullptr,res); DWORD size=SizeofResource(nullptr,res);
    void* ptr=data?LockResource(data):nullptr; if(!ptr||!size) return false;
    HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return false; DWORD wrote=0; BOOL ok=WriteFile(f,ptr,size,&wrote,nullptr); CloseHandle(f);
    return ok && wrote==size;
}
std::wstring payloadDir() {
    static std::wstring dir; if(!dir.empty()) return dir;
    wchar_t tmp[MAX_PATH+1]{}; DWORD n=GetTempPathW(MAX_PATH,tmp);
    dir = (n?std::wstring(tmp):directory(executablePath())+L"\\") + L"thprac-linux\\";
    CreateDirectoryW(dir.c_str(),nullptr);
    // extract embedded deps if present; fall back to exe dir when not embedded
    if(FindResourceW(nullptr,MAKEINTRESOURCEW(IDR_MODULE_DLL),RT_RCDATA)) {
        bool a=writeResource(IDR_FREETYPE_DLL,dir+L"freetype.dll");
        if(a) CopyFileW((dir+L"freetype.dll").c_str(),(dir+L"libfreetype.dll").c_str(),FALSE);
        bool b=writeResource(IDR_MODULE_DLL,dir+L"thprac_th06nc.dll");
        log(std::wstring(L"extract: ")+(a?L"ok":L"fail")+L","+(b?L"ok":L"fail")+L" -> "+dir);
    } else {dir=directory(executablePath())+L"\\"; log(L"no embedded resources, using "+dir);
      std::wstring ftSrc=dir+L"freetype.dll";
      if(GetFileAttributesW(ftSrc.c_str())!=INVALID_FILE_ATTRIBUTES)
        CopyFileW(ftSrc.c_str(),(dir+L"libfreetype.dll").c_str(),FALSE);
    }
    return dir;
}
const wchar_t* localized(int language,const wchar_t* zh,const wchar_t* en,const wchar_t* ja) {
    return language==0?zh:language==2?ja:en;
}
DWORD findGame();
std::wstring envValue(const wchar_t* name) {
    wchar_t buffer[1024]{}; DWORD n=GetEnvironmentVariableW(name,buffer,1024);
    return (n&&n<1024)?std::wstring(buffer):std::wstring();
}
std::wstring posixToWin(std::wstring path) {
    for(auto& c:path)if(c==L'/')c=L'\\';
    if(!path.empty()&&path.front()==L'\\')path=L"Z:"+path;
    return path;
}
std::wstring utf8ToWin(const std::string& text) {
    int n=MultiByteToWideChar(CP_UTF8,0,text.c_str(),(int)text.size(),nullptr,0);
    std::wstring r(n>0?n:0,L'\0');
    if(n>0)MultiByteToWideChar(CP_UTF8,0,text.c_str(),(int)text.size(),r.data(),n);
    return r;
}
std::wstring asciiLower(std::wstring s){for(auto& c:s)if(c>=L'A'&&c<=L'Z')c=wchar_t(c+32);return s;}
bool fileExists(const std::wstring& path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
bool underWine(){return GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version")!=nullptr;}
// Only the game's own prefix may host a child th06nc.exe process; otherwise the
// save data would land in the wrong Proton prefix.
bool prefixIsGame() {
    for(const wchar_t* name:{L"STEAM_COMPAT_DATA_PATH",L"WINEPREFIX"}) {
        auto value=asciiLower(envValue(name));
        if(value.find(L"compatdata/4659620")!=std::wstring::npos||
           value.find(L"compatdata\\4659620")!=std::wstring::npos)return true;
    }
    return false;
}
bool vdfAppLibrary(const std::wstring& vdf,const char* appid,std::wstring& library) {
    HANDLE f=CreateFileW(vdf.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};bool ok=GetFileSizeEx(f,&size)&&size.QuadPart>0&&size.QuadPart<=64*1024*1024;
    std::string text;
    if(ok){text.resize((size_t)size.QuadPart);DWORD got=0;ok=ReadFile(f,text.data(),(DWORD)text.size(),&got,nullptr)&&got==text.size();}
    CloseHandle(f);if(!ok)return false;
    size_t at=text.find(std::string("\"")+appid+"\"");
    if(at==std::string::npos)return false;
    size_t pathKey=text.rfind("\"path\"",at);
    if(pathKey==std::string::npos)return false;
    size_t q=text.find('"',pathKey+6);
    if(q==std::string::npos)return false;
    size_t e=text.find('"',q+1);
    if(e==std::string::npos)return false;
    library=posixToWin(utf8ToWin(text.substr(q+1,e-q-1)));
    return true;
}
// steam://rungameid/4659620 cannot be resolved inside Wine/Proton (the URL
// handler belongs to the Linux Steam client), so locate the Steam library on
// the Z: drive and run th06nc.exe directly instead.
std::wstring resolveGameExe() {
    std::vector<std::wstring> roots;
    for(const wchar_t* name:{L"STEAM_COMPAT_DATA_PATH",L"WINEPREFIX"}) {
        std::wstring value=envValue(name),lower=asciiLower(value);
        while(!lower.empty()&&(lower.back()==L'\\'||lower.back()==L'/')){lower.pop_back();value.pop_back();}
        size_t pos=lower.find(L"\\compatdata\\");
        if(pos==std::wstring::npos)pos=lower.find(L"/compatdata/");
        if(pos!=std::wstring::npos)roots.push_back(posixToWin(value.substr(0,pos)));
    }
    auto home=envValue(L"HOME");
    if(!home.empty()) {
        auto base=posixToWin(home);
        for(auto rel:{L"\\.local\\share\\Steam\\steamapps",L"\\.steam\\steam\\steamapps",
                      L"\\.var\\app\\com.valvesoftware.Steam\\data\\Steam\\steamapps"})
            roots.push_back(base+rel);
    }
    for(auto& root:roots) {
        std::wstring exe=root+L"\\common\\th06nc\\th06nc.exe";
        if(fileExists(exe))return exe;
        std::wstring library;
        if(vdfAppLibrary(root+L"\\libraryfolders.vdf","4659620",library)) {
            exe=library+L"\\steamapps\\common\\th06nc\\th06nc.exe";
            if(fileExists(exe))return exe;
        }
    }
    WIN32_FIND_DATAW fd{};HANDLE h=FindFirstFileW(L"Z:\\home\\*",&fd);
    if(h!=INVALID_HANDLE_VALUE) {
        do {
            if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))continue;
            std::wstring user=std::wstring(L"Z:\\home\\")+fd.cFileName;
            for(auto rel:{L"\\.local\\share\\Steam\\steamapps\\common\\th06nc\\th06nc.exe",
                          L"\\.steam\\steam\\steamapps\\common\\th06nc\\th06nc.exe",
                          L"\\.var\\app\\com.valvesoftware.Steam\\data\\Steam\\steamapps\\common\\th06nc\\th06nc.exe"})
                if(fileExists(user+rel))return user+rel;
            std::wstring library;
            if(vdfAppLibrary(user+L"\\.local\\share\\Steam\\steamapps\\libraryfolders.vdf","4659620",library)) {
                std::wstring exe=library+L"\\steamapps\\common\\th06nc\\th06nc.exe";
                if(fileExists(exe))return exe;
            }
        } while(FindNextFileW(h,&fd));
        FindClose(h);
    }
    return {};
}
bool startGame(const std::wstring& exe) {
    STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};std::wstring cmd=L"\""+exe+L"\"";
    BOOL cp=CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,directory(exe).c_str(),&si,&pi);
    log(std::wstring(L"startDirect exe=")+exe+L" cp="+std::to_wstring(cp)+L" err="+std::to_wstring(GetLastError()));
    if(cp){CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return true;}
    return false;
}
DWORD waitGame(int times) {
    for(int i=0;i<times;++i){DWORD p=findGame();if(p)return p;Sleep(100);}
    return 0;
}
// Direct launch requires either the game's own Proton prefix (a non-Steam
// shortcut pointed at its compatdata) or a native Windows environment.
bool mayDirectLaunch(){return !underWine()||prefixIsGame();}
struct LaunchMapping {
    HANDLE handle{};LaunchOptions* view{};
    ~LaunchMapping(){if(view)UnmapViewOfFile(view);if(handle)CloseHandle(handle);}
    bool create(DWORD pid,const LaunchOptions& options){
        handle=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(LaunchOptions),launchName(pid).c_str());
        if(!handle||GetLastError()==ERROR_ALREADY_EXISTS)return false;
        view=static_cast<LaunchOptions*>(MapViewOfFile(handle,FILE_MAP_ALL_ACCESS,0,0,sizeof(LaunchOptions)));
        if(!view)return false;
        *view=options;return true;
    }
};
uintptr_t remoteModule(DWORD pid,const std::wstring& name) {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if(snapshot==INVALID_HANDLE_VALUE)return 0;
    MODULEENTRY32W item{sizeof(item)};uintptr_t result=0;
    if(Module32FirstW(snapshot,&item))do{if(_wcsicmp(item.szModule,name.c_str())==0){result=(uintptr_t)item.modBaseAddr;break;}}while(Module32NextW(snapshot,&item));
    CloseHandle(snapshot);return result;
}
bool attach(DWORD pid,const LaunchOptions& options) {
    if(shared){if(pid==targetPid)return true;disconnect();}
    LaunchOptions opts=options;
    HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_CREATE_THREAD|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if(!process){failure(L"无法打开游戏进程，错误 "+std::to_wstring(GetLastError()));return false;}
    wchar_t path[32768];DWORD length=32768;
    log(std::wstring(L"attach pid=")+std::to_wstring(pid));
    GameBuild build=GameBuild::Unsupported;
    if(!QueryFullProcessImageNameW(process,0,path,&length)||(build=identifyBuild(path))==GameBuild::Unsupported){ log(std::wstring(L"unsupported: ")+path);
      CloseHandle(process);failure(L"EXE 的 SHA-256 不在已适配的新典版本中，未注入。");return false;}
    opts.build=(int)build; log(L"build="+std::to_wstring(opts.build));
    {auto n=mapName(pid); log(std::wstring(L"open map: ")+n);}
    mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,mapName(pid).c_str());
    LaunchMapping launch;
    HANDLE preMap{}, preMutex{};
    if(!mapping) {
      preMap=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),mapName(pid).c_str());
      preMutex=CreateMutexW(nullptr,FALSE,mutexName(pid).c_str());
      log(std::wstring(L"preMap=")+std::to_wstring((uintptr_t)preMap)+L" preMutex="+std::to_wstring((uintptr_t)preMutex)+L" err="+std::to_wstring(GetLastError()));
      if(!launch.create(pid,opts)){CloseHandle(process);if(preMap)CloseHandle(preMap);if(preMutex)CloseHandle(preMutex);failure(L"Unable to prepare the game launch options. Please close the game and try again.");return false;}
      std::wstring base=payloadDir();
      // ensure freetype.dll is findable as a dependency of thprac_th06nc.dll
      {
        std::wstring ftSrc=base+L"freetype.dll", gameDir=directory(path)+L"\\";
        if(GetFileAttributesW(ftSrc.c_str())==INVALID_FILE_ATTRIBUTES){
          for(auto name:{L"freetype.dll",L"libfreetype.dll"}){
            std::wstring dst=gameDir+name;
            CopyFileW(ftSrc.c_str(),dst.c_str(),FALSE);
          }
        }
      }
      for (const wchar_t* dependency : {L"libfreetype.dll", L"thprac_th06nc.dll"}) {
      std::wstring dll=base+dependency;
      if(GetFileAttributesW(dll.c_str())==INVALID_FILE_ATTRIBUTES){
        // freetype may be named freetype.dll in payload
        if(_wcsicmp(dependency,L"libfreetype.dll")==0) dll=base+L"freetype.dll";
      }
      if(GetFileAttributesW(dll.c_str())==INVALID_FILE_ATTRIBUTES){CloseHandle(process);if(preMap)CloseHandle(preMap);if(preMutex)CloseHandle(preMutex);failure(L"缺少 thprac_th06nc.dll / freetype.dll");return false;}
      auto load=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
      MEMORY_BASIC_INFORMATION info{};VirtualQuery((void*)load,&info,sizeof(info));
      std::wstring localOwner=executablePath((HMODULE)info.AllocationBase);
      auto ownerName=localOwner.substr(localOwner.find_last_of(L"\\")+1);
      auto remoteOwner=remoteModule(pid,ownerName);
      // Steam may expose the PID before its loader has populated the module
      // list (ERROR_PARTIAL_COPY). Wait for LoadLibraryW's owning module;
      // ASLR and process visibility alone do not imply injection readiness.
      for(int i=0;!remoteOwner && i<250;++i) {
        if(WaitForSingleObject(process,20)!=WAIT_TIMEOUT)break;
        remoteOwner=remoteModule(pid,ownerName);
      }
      if(!remoteOwner){CloseHandle(process);if(preMap)CloseHandle(preMap);if(preMutex)CloseHandle(preMutex);failure(L"The game loader is not ready. Please launch the game again.");return false;}
      auto remoteLoad=remoteOwner+((uintptr_t)load-(uintptr_t)info.AllocationBase);
      SIZE_T bytes=(dll.size()+1)*sizeof(wchar_t);void* remote=VirtualAllocEx(process,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
      SIZE_T written=0;bool ok=remote&&WriteProcessMemory(process,remote,dll.c_str(),bytes,&written)&&written==bytes;
      HANDLE thread=ok&&remoteOwner?CreateRemoteThread(process,nullptr,0,(LPTHREAD_START_ROUTINE)remoteLoad,remote,0,nullptr):nullptr;
      DWORD wait=thread?WaitForSingleObject(thread,10000):WAIT_FAILED;
      DWORD exitCode=0; if(thread) GetExitCodeThread(thread,&exitCode);
      if(thread)CloseHandle(thread);
      // Do not free the argument if LoadLibrary is still using it.
      if(remote&&wait!=WAIT_TIMEOUT)VirtualFreeEx(process,remote,0,MEM_RELEASE);
      log(std::wstring(L"inject ")+dependency+L" wait="+std::to_wstring(wait)+L" err="+std::to_wstring(GetLastError())+L" ret="+std::to_wstring((unsigned long long)exitCode));
      if(wait!=WAIT_OBJECT_0 || exitCode==0){CloseHandle(process);if(preMap)CloseHandle(preMap);if(preMutex)CloseHandle(preMutex);failure(L"注入失败或模块加载失败: "+std::wstring(dependency));return false;}
      }
      if(preMap){ mapping=preMap; preMap=nullptr; }
      if(preMutex){ mutex=preMutex; preMutex=nullptr; }
      if(!mapping) for(int i=0;i<400&&!mapping;i++){Sleep(50);mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,mapName(pid).c_str());}
      if(!mapping) log(L"mapping poll timeout err="+std::to_wstring(GetLastError()));
    }
    CloseHandle(process);
    if(!mapping){failure(L"DLL 未建立通信通道。");return false;}
    log(L"mapping opened");
    shared=(Shared*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared));
    if(!shared) log(L"map view failed err="+std::to_wstring(GetLastError()));
    if(!mutex){ mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,mutexName(pid).c_str());
      if(!mutex){ log(L"mutex open failed err="+std::to_wstring(GetLastError())); mutex=OpenMutexW(SYNCHRONIZE,FALSE,mutexName(pid).c_str()); }
    }
    if(!shared||!mutex){failure(L"无法打开通信通道。");return false;}
    for(int i=0;i<400&&shared->magic!=Magic;++i)Sleep(25);
    if(shared->magic!=Magic||shared->version!=Protocol){failure(L"游戏中已加载其他协议版本的练习器，请退出游戏后重新连接。");disconnect();return false;}
    if(opts.practice && !shared->practiceEnabled) {
      failure(localized(opts.language,L"本次游戏仅启用了低延迟显示。请退出游戏，勾选“应用 thprac”后重新启动。",
        L"This game was started without practice features. Close it, enable Apply thprac, and launch again.",
        L"練習機能なしで起動されています。ゲームを終了し、「thpracを適用する」を有効にして起動し直してください。"));
      disconnect();return false;
    }
    for(int i=0;i<400&&!shared->status.ready&&!shared->status.error;++i)Sleep(25);
    log(L"readyState="+std::to_wstring(shared->status.ready)+L" err="+std::to_wstring(shared->status.error));
    if(!shared->status.ready){failure(shared->status.message);disconnect();return false;}
    targetPid=pid;return true;
}
DWORD findGame() {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W entry{sizeof(entry)};DWORD pid=0;
    if(Process32FirstW(snapshot,&entry))do{if(_wcsicmp(entry.szExeFile,L"th06nc.exe")==0){if(pid){pid=0;break;}pid=entry.th32ProcessID;}}while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return pid;
}
// Run a host-side shell command (Wine/Proton/Steam/gamescope, ...) that starts
// the game. Requires a Unix /bin/sh reachable through the Z: drive, i.e. a
// Wine/Proton environment. THPRAC_LAUNCH_CMD holds the command in UTF-8.
bool startViaCommand() {
    auto envCmd=envValue(L"THPRAC_LAUNCH_CMD");
    if(envCmd.empty())return false;
    int n=WideCharToMultiByte(CP_UTF8,0,envCmd.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string utf8(n>1?n-1:0,'\0');
    if(n>1)WideCharToMultiByte(CP_UTF8,0,envCmd.c_str(),-1,utf8.data(),n,nullptr,nullptr);
    std::wstring cmd=utf8ToWin(utf8);
    std::wstring escaped;
    for(auto c:cmd){if(c==L'"'){escaped+=L"\\\"";}else escaped+=c;}
    log(L"startViaCommand sh -c "+cmd);
    // Wine starts non-PE (Unix) executables natively, same mechanism the
    // launcher's cross-prefix attach helper relies on.
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    std::wstring shCmd=L"\"Z:\\bin\\sh\" -c \""+escaped+L"\"";
    bool started=CreateProcessW(L"Z:\\bin\\sh",shCmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi);
    auto result=started?INT_PTR(1):0;
    if(started){CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
    else {
      log(L"startViaCommand CreateProcessW err="+std::to_wstring(GetLastError()));
      std::wstring params=L"-c \""+escaped+L"\"";
      result=(INT_PTR)ShellExecuteW(nullptr,L"open",L"Z:\\bin\\sh",params.c_str(),nullptr,SW_HIDE);
    }
    log(L"startViaCommand result="+std::to_wstring(result));
    return result>32;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
 int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);DWORD pid=0;bool launch=false;
 LaunchOptions options;
 int language=1;
 switch(PRIMARYLANGID(GetUserDefaultUILanguage())) {case LANG_CHINESE:language=0;break;case LANG_JAPANESE:language=2;break;}
 for(int i=1;i<argc;++i) {
   if(std::wstring(argv[i])==L"--attach"&&i+1<argc)pid=wcstoul(argv[++i],nullptr,10);
   else if(std::wstring(argv[i])==L"--steam")launch=true;
   else if(std::wstring(argv[i])==L"--low-latency")options.lowLatency=1;
   else if(std::wstring(argv[i])==L"--no-practice")options.practice=0;
   else if(std::wstring(argv[i])==L"--language") {
     if(i+1>=argc||wcslen(argv[i+1])!=1||argv[i+1][0]<L'0'||argv[i+1][0]>L'2') {
       LocalFree(argv);failure(L"Invalid --language (0=Chinese, 1=English, 2=Japanese).");return 4;
     }
     language=argv[++i][0]-L'0';
   }
 }
 LocalFree(argv);
 options.language=language;
 // Child processes inherit this environment. A non-Steam shortcut under Proton sets
 // SteamAppId/SteamGameId to the shortcut's own ID, which makes steam_api64 reject the
 // game and exit right after launch. Force the th06nc AppID; launching from the real
 // Steam entry already has this value, so nothing changes there.
 for(const wchar_t* name:{L"SteamAppId",L"SteamGameId"}) {
   wchar_t old[64]{}; GetEnvironmentVariableW(name,old,63);
   log(std::wstring(L"env ")+name+L"="+old+L" -> 4659620");
   SetEnvironmentVariableW(name,L"4659620");
 }
  log(L"start: exe="+executablePath()+L" pidArg="+std::to_wstring(pid)+L" launch="+(launch?L"1":L"0"));
 if(launch) {
   if(options.lowLatency && findGame()) {
     failure(localized(language,L"低延迟选项在启动时生效。请先退出正在运行的新典，再从启动器启动。",
       L"Low latency is a launch option. Close the running game, then launch it again from thprac.",
       L"低遅延モードは起動時に適用されます。起動中のゲームを終了し、thpracから起動し直してください。"));return 5;
   }
    if(!findGame()) {
      bool started=startViaCommand();
      if(!started) {
        std::wstring exe=directory(executablePath())+L"\\th06nc.exe";
        if(fileExists(exe))started=startGame(exe);
        else log(L"no th06nc.exe at "+exe);
        if(!started) {
          if(mayDirectLaunch()) {
            std::wstring resolved=resolveGameExe();
            if(resolved.empty())log(L"steam library resolution failed");
            else started=startGame(resolved);
          } else log(L"prefix is not the game's own, skip direct launch");
        }
        if(!started && (INT_PTR)ShellExecuteW(nullptr,L"open",L"steam://rungameid/4659620",nullptr,nullptr,SW_SHOWNORMAL)<=32) {failure(L"无法启动游戏。");return 1;}
      }
    }
    pid=waitGame(600);
  }
  if(!pid)pid=findGame();
  if(!pid) {
    bool started=startViaCommand();
    if(!started) {
      std::wstring exe=directory(executablePath())+L"\\th06nc.exe";
      if(fileExists(exe))started=startGame(exe);
      else log(L"no th06nc.exe at "+exe);
      if(!started&&mayDirectLaunch()) {
        std::wstring resolved=resolveGameExe();
        if(resolved.empty())log(L"steam library resolution failed");
        else started=startGame(resolved);
      }
    }
    if(started)pid=waitGame(600);
  }
 log(L"pid="+std::to_wstring(pid)); if(!pid){failure(L"未找到 th06nc 游戏进程。");return 2;}
 bool ok=attach(pid,options);
 if(ok) {
   auto lock=WaitForSingleObject(mutex,1000);
   if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED){shared->language=language;ReleaseMutex(mutex);}
   else {failure(L"Unable to synchronize the practice UI language.");ok=false;}
 }
 if(ok && options.lowLatency) {
   for(int i=0;i<400 && shared->lowLatencyState==static_cast<int>(THPrac::TH06NC::DisplayState::Pending);++i)Sleep(50);
   if(shared->lowLatencyState!=static_cast<int>(THPrac::TH06NC::DisplayState::Active)) {
     const wchar_t* text=shared->lowLatencyState==static_cast<int>(THPrac::TH06NC::DisplayState::Unavailable)
       ?localized(language,L"低延迟显示未能启用，游戏将使用原本的显示方式。请使用 Borderless 或 Window；当前系统及显卡驱动也需支持此显示模式。",
         L"Low-latency display could not be enabled. The game will use its original display path. Use Borderless or Window; support from Windows and the graphics driver is also required.",
         L"低遅延表示を有効にできなかったため、通常の表示方式を使用します。Borderless または Window を選択してください。Windowsとグラフィックスドライバーの対応も必要です。")
       :localized(language,L"尚未确认低延迟显示已就绪。请关闭游戏后重新启动。",
         L"Low-latency display is not ready. Please close the game and launch it again.",
         L"低遅延表示の準備を確認できませんでした。ゲームを終了して起動し直してください。");
     MessageBoxW(nullptr,text,L"thprac-linux",MB_ICONWARNING);
   }
 }
 disconnect();return ok?0:3;
}
#else
// th06nc practice module DLL.

#include "MinHook.h"
#include "thprac_locale_def.h"
#include "thprac_gui_components.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <memory>
#include <utility>
#include <stdexcept>
#include <atomic>
#include <cstring>

namespace THPrac::TH06NC { uintptr_t imageBase{}; }
using namespace THPrac::TH06NC;

// Shared practice parameters (th06nc/practice_param.h)
namespace THPrac::TH06NC {
    struct THPracParam {
        int32_t mode;

        int32_t stage;
        int32_t section;
        int32_t phase;
        int32_t frame;

        int64_t score;
        float life;
        float bomb;
        float power;
        int32_t graze;
        int32_t point;

        int32_t rank;
        bool customRank;
        int32_t fakeType;

        bool dlg;

        bool _playLock;
        void Reset()
        {
            mode = 0;
            stage = 0;
            section = 0;
            phase = 0;
            score = 0ll;
            life = 0.0f;
            bomb = 0.0f;
            power = 0.0f;
            graze = 0;
            point = 0;
            rank = 0;
            customRank = false;
            fakeType = 0;
            dlg = false;
            frame = 0;
        }
    };
}
// Game RVAs for both builds (th06nc/addresses.h)
// Original 1.03 defaults. Select the Steam build before installing any hooks.
// Both the bridge and module identify the complete EXE by SHA-256.
namespace THPrac::TH06NC::Rva {
inline uintptr_t GameInit=0x3a9c0;
inline uintptr_t GameUpdate=0x3a210;
inline uintptr_t PlayerUpdate=0x68820;
inline uintptr_t EnemyUpdate=0x373b0;
inline uintptr_t EclUpdate=0x22060;
inline uintptr_t FinalSpellRage=0x35850;
inline uintptr_t MenuUpdate=0x479e0;
inline uintptr_t MenuTransition=0x4da80;
inline uintptr_t PlayBgm=0x7bc80;
inline uintptr_t StopSound=0xcce50;
inline uintptr_t PlaySound=0xc8110;
inline uintptr_t SoundTable=0;
inline uintptr_t BgmHandle=0x50966c;
inline uintptr_t BgmLoop=0x50967d;
inline uintptr_t WriteFile=0x39610;
inline uintptr_t SupervisorUpdate=0x79c70;
inline uintptr_t LoadAnm=0x20b0;
inline uintptr_t ResultInit=0x73d50;
inline uintptr_t ExtendScores=0x3084c8;
inline uintptr_t NativeRanks=0x3084e0;
inline uintptr_t WaitFactor=0x30cd90;
inline uintptr_t Misses=0x4f1e60;
inline uintptr_t BombsUsed=0x4f1e64;
inline uintptr_t Character=0x4f1e80;
inline uintptr_t Shot=0x4f1e81;
inline uintptr_t Stage=0x4f1e84;
inline uintptr_t Power=0x4f1e88;
inline uintptr_t Replay=0x4f278c;
inline uintptr_t DisplayScore=0x4f2790;
inline uintptr_t Score=0x4f2798;
inline uintptr_t Paused=0x4f27b0;
inline uintptr_t GameOver=0x4f27b1;
inline uintptr_t Practice=0x4f27b4;
inline uintptr_t SpellPractice=0x4f27b5;
inline uintptr_t StagePoint=0x4f27ba;
inline uintptr_t Point=0x4f27bc;
inline uintptr_t Difficulty=0x4f27c0;
inline uintptr_t StageGraze=0x4ff0cc;
inline uintptr_t Graze=0x4ff0d0;
inline uintptr_t Lives=0x4ff0f0;
inline uintptr_t Bombs=0x4ff0f1;
inline uintptr_t Extends=0x4ff0f2;
inline uintptr_t ReplayPath=0x4ff164;
inline uintptr_t Rank=0x4ff27c;
inline uintptr_t StageData=0x509c08;
inline uintptr_t ReplayManager=0xa6ec38;
inline uintptr_t RandomCount=0xa6ec40;
inline uintptr_t RandomSeed=0xa6ec44;
inline uintptr_t InputRepeat=0xa6ec48;
inline uintptr_t Input=0xa6ec60;
inline uintptr_t PreviousInput=0xa6ec64;
inline uintptr_t KeepBgm=0xaa1e8c;
inline uintptr_t TimelineFrame=0xbadf4c;
inline uintptr_t NextState=0xc21d9c;
inline uintptr_t CurrentState=0xc21d98;
inline uintptr_t CounterFrequency=0xc220f8;
inline uintptr_t FramePeriod=0xc22100;
inline uintptr_t Fps=0xc22108;
inline uintptr_t HalfWait=0xc22118;
inline uintptr_t ResetFrameTimer=0xc22120;
inline uintptr_t EclFile=0xa6eb78;
inline uintptr_t Gui=0xa6ec08;
inline uintptr_t Player=0x4ff3a0;
// DXLib D3D11 display state, independently checked against 1.03's resize path.
inline uintptr_t CalcChain=0x3be80;
inline uintptr_t SetRenderTarget=0x2431b0;
inline uintptr_t ReleaseBackbuffer=0x2539c0;
inline uintptr_t SwapChain=0x8fe3b0;
inline uintptr_t SwapChainDesc=0x8fe3b8;
inline uintptr_t Output=0x8fe400;
inline uintptr_t Output1=0x8fe408;
inline uintptr_t Backbuffer=0x8fe468;
inline uintptr_t BackbufferDesc=0x8fe470;
inline uintptr_t BackbufferSource=0x8fe4a0;
inline uintptr_t BackbufferTarget=0x8fe4a8;
inline uintptr_t CachedRenderTargets=0x93df60;
inline uintptr_t AlternatePresent1=0x56cad4;
inline uintptr_t AlternatePresent2=0x56cad8;

// Steam build 25306795: recompiled code and relocated native globals.
inline void UseSteam25306795() {
    GameInit=0x3c250;
    GameUpdate=0x3baa0;
    PlayerUpdate=0x69f30;
    EnemyUpdate=0x38cd0;
    EclUpdate=0x21f10;
    FinalSpellRage=0x37170;
    MenuUpdate=0x49140;
    MenuTransition=0x4f1e0;
    PlayBgm=0x7d8d0;
    StopSound=0xceb30;
    PlaySound=0xc9df0;
    SoundTable=0x7e2b60;
    BgmHandle=0x5542bc;
    BgmLoop=0x5542cd;
    WriteFile=0x3aea0;
    SupervisorUpdate=0x7b7f0;
    LoadAnm=0x2440;
    ResultInit=0x75420;
    ExtendScores=0x34e0c8;
    NativeRanks=0x34e0e0;
    WaitFactor=0x352a80;
    Misses=0x53cab0;
    BombsUsed=0x53cab4;
    Character=0x53cad0;
    Shot=0x53cad1;
    Stage=0x53cad4;
    Power=0x53cad8;
    Replay=0x53d3dc;
    DisplayScore=0x53d3e0;
    Score=0x53d3e8;
    Paused=0x53d400;
    GameOver=0x53d401;
    Practice=0x53d404;
    SpellPractice=0x53d405;
    StagePoint=0x53d40a;
    Point=0x53d40c;
    Difficulty=0x53d410;
    StageGraze=0x549d1c;
    Graze=0x549d20;
    Lives=0x549d40;
    Bombs=0x549d41;
    Extends=0x549d42;
    ReplayPath=0x549db4;
    Rank=0x549ecc;
    StageData=0x554858;
    ReplayManager=0xabae58;
    RandomCount=0xabae60;
    RandomSeed=0xabae64;
    InputRepeat=0xabae68;
    Input=0xabae80;
    PreviousInput=0xabae84;
    KeepBgm=0xaee0ac;
    TimelineFrame=0xbfa16c;
    NextState=0xc6dfbc;
    CurrentState=0xc6dfb8;
    CounterFrequency=0xc6e318;
    FramePeriod=0xc6e320;
    Fps=0xc6e328;
    HalfWait=0xc6e338;
    ResetFrameTimer=0xc6e340;
    EclFile=0xabad98;
    Gui=0xabae28;
    Player=0x549ff0;
    CalcChain=0x3d700;
    SetRenderTarget=0x245040;
    ReleaseBackbuffer=0x255630;
    SwapChain=0x949350;
    SwapChainDesc=0x949358;
    Output=0x9493a0;
    Output1=0x9493a8;
    Backbuffer=0x949408;
    BackbufferDesc=0x949410;
    BackbufferSource=0x949440;
    BackbufferTarget=0x949448;
    CachedRenderTargets=0x988f00;
    AlternatePresent1=0x5b7a74;
    AlternatePresent2=0x5b7a78;
}
}
// Cross-part declarations (th06nc/module.h)
namespace THPrac::TH06NC {
extern uintptr_t imageBase;
extern THPracParam thPracParam;
int MenuStage();
int MenuDifficulty();
int MenuShot();
int NativeRank(int difficulty);
int PracticeLanguage();
bool InstallOverlay(bool lowLatency = false);
void OpenPractice();
void ClosePractice();
bool PracticeIsOpen();
// Called on the game thread. 0 = pending, 1 = confirm, 2 = cancel.
int TakePracticeAction();
void DrawPractice();
bool ApplyEcl();
int EclFailureOffset();
bool EclNeedsBossAssets();
int NativeTimelineFrame(int stage,int oldTime);
bool PracticeActive();
uint32_t PracticeFlags();
void RequestPracticeRestart();
int PracticeFps();
void SetPracticeFps(int fps);
void ToggleQuickMenu();
void ToggleAdvancedMenu();
void OpenPause();
void ClosePause();
int TakePauseAction();
}
// ---- Low-latency presentation (th06nc/presentation.cpp) ----

namespace THPrac::TH06NC {
namespace {
static_assert(sizeof(void*) == 8 && sizeof(DXGI_SWAP_CHAIN_DESC) == 72 && sizeof(D3D11_TEXTURE2D_DESC) == 44);
using Microsoft::WRL::ComPtr;
using ResizeCallback = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
ResizeCallback originalResize{};
bool enabled{}, submitted{};
std::atomic<DisplayState> state{DisplayState::Off};
std::atomic<HRESULT> lastError{S_OK};
IDXGISwapChain* observed{}; // Borrowed: the game owns the chain.
ComPtr<ID3D11DeviceContext> dxContext;
HANDLE frameAvailable{};

template<class T> T& data(uintptr_t rva) { return *reinterpret_cast<T*>(imageBase + rva); }
void closeWaitHandle()
{
    if (frameAvailable) CloseHandle(frameAvailable);
    frameAvailable = nullptr;
    submitted = false;
}
void unavailable(HRESULT error)
{
    closeWaitHandle();
    dxContext.Reset();
    lastError = error;
    state = DisplayState::Unavailable;
}

HRESULT STDMETHODCALLTYPE resizeBuffers(IDXGISwapChain* swap, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags)
{
    // NC normally passes flags=0. DXGI requires the tearing and waitable-object
    // creation flags to survive ResizeBuffers. Do not alter other swap chains.
    if (enabled && swap == data<IDXGISwapChain*>(Rva::SwapChain)) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(swap->GetDesc(&desc)))
            flags |= desc.Flags & (DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    }
    return originalResize(swap, count, width, height, format, flags);
}

HRESULT convert(IDXGISwapChain* oldSwap, const DXGI_SWAP_CHAIN_DESC& oldDesc)
{
    // The tested NC renderer draws into an offscreen target, then into this
    // window backbuffer. Exclusive fullscreen and multisampled swap chains
    // retain the native path; changing their display mode would be intrusive.
    if (!oldDesc.Windowed || oldDesc.SampleDesc.Count != 1 ||
        oldDesc.SwapEffect != DXGI_SWAP_EFFECT_DISCARD ||
        oldDesc.BufferDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        data<int>(Rva::AlternatePresent1) || data<int>(Rva::AlternatePresent2))
        return DXGI_ERROR_UNSUPPORTED;

    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGIFactory5> features;
    ComPtr<ID3D11Device> device;
    HRESULT hr = oldSwap->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    hr = factory.As(&features);
    if (FAILED(hr)) return hr;
    BOOL tearing = FALSE;
    hr = features->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    if (FAILED(hr) || !tearing) return FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED;
    hr = oldSwap->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr)) return hr;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = oldDesc.BufferDesc.Width;
    desc.Height = oldDesc.BufferDesc.Height;
    desc.Format = oldDesc.BufferDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = oldDesc.BufferUsage;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> replacement;
    // Prepare all objects before changing any game-owned pointer. A failure
    // here leaves the original chain, backbuffer and rendering state intact.
    hr = factory->CreateSwapChainForHwnd(device.Get(), oldDesc.OutputWindow, &desc, nullptr, nullptr, &replacement);
    if (FAILED(hr)) return hr;
    ComPtr<IDXGISwapChain2> queue;
    hr = replacement.As(&queue);
    if (FAILED(hr)) return hr;
    hr = queue->SetMaximumFrameLatency(1);
    if (FAILED(hr)) return hr;
    ComPtr<ID3D11Texture2D> buffer;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutput1> output1;
    DXGI_SWAP_CHAIN_DESC actual{};
    if (FAILED(hr = replacement->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
        FAILED(hr = device->CreateShaderResourceView(buffer.Get(), nullptr, &source)) ||
        FAILED(hr = device->CreateRenderTargetView(buffer.Get(), nullptr, &target)) ||
        FAILED(hr = replacement->GetContainingOutput(&output)) ||
        FAILED(hr = output.As(&output1)) ||
        FAILED(hr = replacement->GetDesc(&actual))) return hr;
    HANDLE available = queue->GetFrameLatencyWaitableObject();
    if (!available) return E_FAIL;

    // Mirror NC's native ResizeBackbuffer (0x253a60): flush/unbind its cached
    // render target slots, release the old backbuffer views, then bind the new
    // views through the game's own cache-aware setter. No ClearState, which
    // would desynchronize the rest of the game's D3D11 state cache.
    auto setTarget = reinterpret_cast<int64_t(*)(void*, void*, unsigned)>(imageBase + Rva::SetRenderTarget);
    auto releaseBuffers = reinterpret_cast<int64_t(*)(unsigned)>(imageBase + Rva::ReleaseBackbuffer);
    auto oldBuffer = data<ID3D11Texture2D*>(Rva::Backbuffer);
    bool bound[8]{};
    for (unsigned i = 0; i < 8; ++i) {
        bound[i] = data<ID3D11Texture2D*>(Rva::CachedRenderTargets + i * sizeof(void*)) == oldBuffer;
        if (bound[i]) setTarget(nullptr, nullptr, i);
    }
    releaseBuffers(0);
    data<IDXGISwapChain*>(Rva::SwapChain) = replacement.Detach();
    data<DXGI_SWAP_CHAIN_DESC>(Rva::SwapChainDesc) = actual;
    data<ID3D11Texture2D*>(Rva::Backbuffer) = buffer.Detach();
    data<ID3D11Texture2D*>(Rva::Backbuffer)->GetDesc(&data<D3D11_TEXTURE2D_DESC>(Rva::BackbufferDesc));
    data<ID3D11ShaderResourceView*>(Rva::BackbufferSource) = source.Detach();
    data<ID3D11RenderTargetView*>(Rva::BackbufferTarget) = target.Detach();
    if (auto old = data<IDXGIOutput*>(Rva::Output)) old->Release();
    if (auto old = data<IDXGIOutput1*>(Rva::Output1)) old->Release();
    data<IDXGIOutput*>(Rva::Output) = output.Detach();
    data<IDXGIOutput1*>(Rva::Output1) = output1.Detach();
    for (unsigned i = 0; i < 8; ++i)
        if (bound[i]) setTarget(data<void*>(Rva::Backbuffer), data<void*>(Rva::BackbufferTarget), i);
    oldSwap->Release();

    closeWaitHandle();
    frameAvailable = available;
    device->GetImmediateContext(dxContext.ReleaseAndGetAddressOf());
    observed = data<IDXGISwapChain*>(Rva::SwapChain);
    submitted = true;
    lastError = S_OK;
    state = DisplayState::Active;
    return S_OK;
}
}

bool InstallLowLatency(void* resizeBuffersEntry)
{
    if (MH_CreateHook(resizeBuffersEntry, reinterpret_cast<void*>(resizeBuffers),
        reinterpret_cast<void**>(&originalResize)) != MH_OK) return false;
    enabled = true;
    state = DisplayState::Pending;
    return true;
}

void LowLatencyBeforeFrame()
{
    if (!enabled) return;
    auto swap = data<IDXGISwapChain*>(Rva::SwapChain);
    if (!swap || !data<ID3D11Texture2D*>(Rva::Backbuffer)) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc))) return;
    // Graphics options can destroy/recreate a chain (even at the same address).
    if (swap != observed || (state == DisplayState::Active && desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD)) {
        closeWaitHandle();
        dxContext.Reset();
        observed = swap;
        if (IsIconic(desc.OutputWindow) || !desc.BufferDesc.Width || !desc.BufferDesc.Height) {
            observed = nullptr;
            return;
        }
        HRESULT hr = convert(swap, desc);
        if (FAILED(hr)) unavailable(hr);
    }
    if (state == DisplayState::Active && submitted && frameAvailable) {
        submitted = false;
        // Once per rendered frame, BEFORE the calc chain samples input. Replay
        // fast-forward may run this chain several times without a Present.
        // Wake for window messages and bound the wait to keep Alt-Tab/minimize
        // responsive; the game's normal message pump still dispatches them.
        if (!IsIconic(desc.OutputWindow) && GetForegroundWindow() == desc.OutputWindow)
            MsgWaitForMultipleObjectsEx(1, &frameAvailable, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

HRESULT LowLatencyPresent(IDXGISwapChain* swap, UINT interval, UINT flags, PresentCallback original)
{
    if (state != DisplayState::Active || swap != observed || !dxContext || (flags & DXGI_PRESENT_TEST))
        return original(swap, interval, flags);
    BOOL fullscreen = FALSE;
    if (FAILED(swap->GetFullscreenState(&fullscreen, nullptr))) return original(swap, interval, flags);
    if (!fullscreen) flags |= DXGI_PRESENT_ALLOW_TEARING;
    else flags &= ~DXGI_PRESENT_ALLOW_TEARING;

    // Flip Present unbinds the backbuffer RTV. Restore the actual bindings so
    // NC's cached setters remain valid on the next frame, including its UI.
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* depth{};
    dxContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
    HRESULT result = original(swap, 0, flags);
    dxContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth);
    for (auto target : targets) if (target) target->Release();
    if (depth) depth->Release();
    submitted = SUCCEEDED(result);
    return result;
}

DisplayState LowLatencyState() { return state.load(); }
uint32_t LowLatencyError() { return static_cast<uint32_t>(lastError.load()); }
}
// ---- ECL patching (th06nc/ecl.cpp) ----

namespace THPrac::TH06NC {
using namespace TH06;
using std::pair;
namespace {
int warpedFrame{};
int failureOffset{};
bool needsBossAssets{};
int TranslateFrame(int oldTime) {
    for(auto& map:frameMap)if(map.stage==thPracParam.stage+1 && map.oldTime>=oldTime)
        return map.newTime+(oldTime-map.oldTime);
    throw std::runtime_error("Missing timeline anchor");
}
class ECLHelper {
    struct Write { int oldOffset; std::vector<unsigned char> bytes; };
    std::vector<Write> writes;
    struct NativeWrite { int offset,opcode,part; bool timeline; std::vector<unsigned char> bytes; };
    std::vector<NativeWrite> nativeWrites;
    int position{};
public:
    template<class T> void Native(int offset,int opcode,int part,bool timeline,T value) {
        static_assert(std::is_integral_v<T>);
        auto data=reinterpret_cast<unsigned char*>(&value);
        nativeWrites.push_back({offset,opcode,part,timeline,{data,data+sizeof(T)}});
    }
    void SetPos(int p) { position=p; }
    template<class T> ECLHelper& operator<<(T value) {
        static_assert(std::is_integral_v<T>);
        auto p=reinterpret_cast<unsigned char*>(&value);
        writes.push_back({position,{p,p+sizeof(T)}});
        position+=sizeof(T);return *this;
    }
    template<class T> ECLHelper& operator<<(pair<int,T> value) {
        SetPos(value.first);return *this<<value.second;
    }
    bool Apply() {
        auto script=*reinterpret_cast<unsigned char**>(imageBase+Rva::EclFile);
        if(!script)return false;
        int stage=thPracParam.stage+1;
        struct Translated { unsigned char* dest; std::vector<unsigned char> bytes; };
        std::vector<Translated> pending,nativePending;
        for(auto& write:nativeWrites){
            auto size=scriptGuards[stage-1].size;
            if(write.offset<0 || size_t(write.offset)+12>size || write.part<0)return false;
            uint16_t opcode,length;memcpy(&opcode,script+write.offset+4,2);memcpy(&length,script+write.offset+6,2);
            if(opcode!=write.opcode || length<(write.timeline?8:12) || size_t(write.offset)+length>size ||
               size_t(write.part)+write.bytes.size()>length)return false;
            nativePending.push_back({script+write.offset+write.part,write.bytes});
        }
        for(auto& write:writes) {
            // NC moved this damage-enable instruction out of the final-card
            // prelude into its setup sub. There is no prelude timestamp to edit.
            if(stage==7 && write.oldOffset==0xbe90 && write.bytes.size()==4)continue;
            const EclMap* found=nullptr;
            for(auto& map:eclMap) if(map.stage==stage && write.oldOffset>=map.oldOffset &&
                write.oldOffset+int(write.bytes.size())<=map.oldOffset+map.oldSize) {found=&map;break;}
            if(!found || write.oldOffset-found->oldOffset+int(write.bytes.size())>found->newSize){failureOffset=write.oldOffset;return false;}
            pending.push_back({script+found->newOffset+(write.oldOffset-found->oldOffset),write.bytes});
            if(found->timeline && write.oldOffset==found->oldOffset && write.bytes.size()==2){
                int16_t oldTime;memcpy(&oldTime,write.bytes.data(),2);
                // Zero means skip this event, not a frame before the new stage.
                int16_t newTime=oldTime==0?0:int16_t(TranslateFrame(oldTime));
                memcpy(pending.back().bytes.data(),&newTime,2);
            }
            if(!found->timeline && write.oldOffset==found->oldOffset && write.bytes.size()==4)
                for(auto& extra:extraTimeMap)if(extra.stage==stage && extra.oldFollowing==write.oldOffset){
                    int before,after;memcpy(&before,script+extra.newOffset,4);memcpy(&after,write.bytes.data(),4);
                    if(after<before)pending.push_back({script+extra.newOffset,write.bytes});
                }
        }
        // Translate call operands after all recipe writes have supplied their new opcode.
        for(auto& map:eclMap) if(map.stage==stage && !map.timeline && map.oldSize>=12) {
            auto instruction=script+map.newOffset;
            uint16_t opcode;memcpy(&opcode,instruction+4,2);
            for(auto& p:pending) if(p.dest<=instruction+4 && p.dest+p.bytes.size()>=instruction+6)
                memcpy(&opcode,p.bytes.data()+(instruction+4-p.dest),2);
            bool subRef=opcode==35||(opcode>=37&&opcode<=42)||opcode==95||opcode==108||opcode==109||opcode==114||opcode==116;
            if(subRef)for(auto& p:pending)if(p.dest==instruction+12&&p.bytes.size()==4) {
                int sub;memcpy(&sub,p.bytes.data(),4);bool translated=sub<0;
                for(auto& entry:subMap)if(entry.stage==stage&&entry.oldSub==sub){sub=entry.newSub;translated=true;break;}
                if(!translated){failureOffset=map.oldOffset+12;return false;}
                memcpy(p.bytes.data(),&sub,4);
            }
        }
        pending.insert(pending.end(),nativePending.begin(),nativePending.end());
        for(auto& p:pending)memcpy(p.dest,p.bytes.data(),p.bytes.size());
        return true;
    }
};
void ECLWarp(int32_t oldTime) {
    warpedFrame=TranslateFrame(oldTime);
}
void ECLSetHealth(ECLHelper& ecl,int offset,int32_t time,int32_t hp) {
    ecl.SetPos(offset);ecl<<time<<0x0010006f<<0x00ffff00<<hp;
}
void ECLSetTime(ECLHelper& ecl,int offset,int32_t time,int32_t timer) {
    ecl.SetPos(offset);ecl<<time<<0x00100073<<0x00ffff00<<timer;
}
void ECLStall(ECLHelper& ecl,int offset) {
    ecl.SetPos(offset);ecl<<0x99999<<0x000c0000<<0x0000ff00;
}
// These assets are normally loaded by dialogue; custom practice skips it.
void ECLNameFix() { needsBossAssets=true; }
// Original TH06 practice patch recipes, translated by the verified ECL address map.
    TH_NOINLINE void THPatch(ECLHelper& ecl, th_sections_t section)
    {
        int shot;
        auto s2b_nd = [&]() {
            ECLWarp(0x1760);
            ecl << pair{0x18fc, 0x0};
            ecl << pair{0x191c, 0x0};
            ecl << pair{0x192c, 0x0};
            ecl << pair{0x194c, 0x0};
            ecl << pair{0x196c, 0x0};
            ecl << pair{0x198c, 0x0};
            ecl << pair{0x19a0, 0x0};
        };
        auto s3b_n1 = [&]() {
            ecl << pair{0x1274, (int16_t)0x0};
            ecl << pair{0x12f0, (int16_t)0x0};
            ECLWarp(0x16d4);
            ecl << pair{0x80d6, (int16_t)0x16d4};
            ecl << pair{0x1f70, 0x1};
            ecl << pair{0x1f90, 0x0};
            ecl << pair{0x80dc, (int16_t)0x24};
            ecl << pair{0x20ec, 0x0};
            ecl << pair{0x210c, 0x0};
            ecl << pair{0x212c, 0x0};
            ecl << pair{0x214c, 0x1e};
            ecl << pair{0x2160, 0x1e};
            ecl << pair{0x2194, 0x1e};
            ecl << pair{0x2188, 150};
        };
        auto s4b_time = [&]() {
            ecl << pair{0x2790, 0x0};
            ecl << pair{0x27b0, 0x0};
            ecl << pair{0x27d0, 0x0};
            ecl << pair{0x27f0, 0x0};
            ecl << pair{0x2810, 0x0};
            ecl << pair{0x2824, 0x0};
            ecl << pair{0x283c, 0x0};
            ecl << pair{0x2850, 0x0};
        };
        auto s7b_call = [&]() {
            ecl << pair{0x344e, 0x0};
            ecl << pair{0x3452, 0x00180023};
            ecl << pair{0x3456, 0x00ffff00};
            ecl << pair{0x345a, 0x0};
            ecl << pair{0x345e, 0x0};
            ecl << pair{0x3462, 0x0};
            ECLStall(ecl, 0x3466);
        };
        auto s7b_n1 = [&]() {
            ECLWarp(0x2192);
            ecl << pair{0x339e, 0x0};
            ecl << pair{0x33ae, 0x0};
            ecl << pair{0x33ce, 0x0};
            ecl << pair{0x33ee, 0x0};
            ecl << pair{0x340e, 0x0};
            ecl << pair{0x342e, 0x0};
        };

        switch (section) {
        case THPrac::TH06::TH06_ST1_MID1:
            ECLWarp(0x7d8);
            ecl << pair{0x0ab0, 0x3c} << pair{0x0ad0, 0x3c};
            break;
        case THPrac::TH06::TH06_ST1_MID2:
            ECLWarp(0x7d8);
            ecl << pair{0x0ab0, 0x3c} << pair{0x0ad0, 0x3c};
            ECLSetHealth(ecl, 0x0af0, 0x3c, 0x1f3);
            break;
        case THPrac::TH06::TH06_ST1_BOSS1:
            if (thPracParam.dlg)
                ECLWarp(0x149e);
            else {
                ECLWarp(0x149f);
                ecl << pair{0x16a6, 0} << pair{0x16c6, 0} << pair{0x16e6, 0x50};
            }
            break;
        case THPrac::TH06::TH06_ST1_BOSS2:
            ECLWarp(0x149f);
            ecl << pair{0x16a6, 0} << pair{0x16c6, 0} << pair{0x16e6, 0x50};
            ECLSetTime(ecl, 0x16e6, 0, 0);
            ECLStall(ecl, 0x16f6);
            break;
        case THPrac::TH06::TH06_ST1_BOSS3:
            ECLWarp(0x149f);
            ecl << pair{0x16a6, 0} << pair{0x16c6, 0} << pair{0x16e6, 0x50}
                << pair{0x16f2, 0x10} << pair{0x293a, 0} << pair{0x294a, 0}
                << pair{0x291e, (int16_t)0};
            break;
        case THPrac::TH06::TH06_ST1_BOSS4:
            ECLWarp(0x149f);
            ecl << pair{0x16a6, 0} << pair{0x16c6, 0} << pair{0x16e6, 0x50}
                << pair{0x16f2, 0x10} << pair{0x293a, 0} << pair{0x294a, 0}
                << pair{0x291e, (int16_t)0};
            ECLSetTime(ecl, 0x294a, 0, 0);
            ECLStall(ecl, 0x295a);
            break;
        case THPrac::TH06::TH06_ST2_MID1:
            ECLWarp(0xa1c);
            break;
        case THPrac::TH06::TH06_ST2_BOSS1:
            if (thPracParam.dlg)
                ECLWarp(0x175f);
            else
                ECLWarp(0x1760);
            break;
        case THPrac::TH06::TH06_ST2_BOSS2:
            s2b_nd();
            ECLSetTime(ecl, 0x19a0, 0x0, 0x0);
            ECLStall(ecl, 0x19b0);
            break;
        case THPrac::TH06::TH06_ST2_BOSS3:
            s2b_nd();
            ecl << pair{0x19ac, 0x19};
            ecl << pair{0x2138, 0x0};
            ecl << pair{0x2148, 0x60};
            ecl << pair{0x2110, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST2_BOSS4:
            s2b_nd();
            ecl << pair{0x19ac, 0x19};
            ecl << pair{0x2138, 0x0};
            ecl << pair{0x2148, 0x60};
            ecl << pair{0x2110, (int16_t)0x0};
            ECLSetTime(ecl, 0x2148, 0x30, 0x0);
            ECLStall(ecl, 0x2158);
            break;
        case THPrac::TH06::TH06_ST2_BOSS5:
            s2b_nd();
            ecl << pair{0x19ac, 0x19};
            ecl << pair{0x2138, 0x0};
            ecl << pair{0x2148, 0x60};
            ecl << pair{0x2110, (int16_t)0x0};
            ecl << pair{0x2148, 0x0};
            ecl << pair{0x2154, 0x20};
            ecl << pair{0x33a2, (int16_t)0x0};
            ecl << pair{0x337a, (int16_t)0x0};
            ecl << pair{0x3392, (int16_t)0x0};
            ecl << pair{0x2090, 0x578};
            ecl << pair{0x20b0, 0xffffffff};
            ecl << pair{0x20c0, 0xffffffff};
            ecl << pair{0x20f0, 0x1c};
            break;
        case THPrac::TH06::TH06_ST3_MID1:
            ECLWarp(0x0edc);
            break;
        case THPrac::TH06::TH06_ST3_MID2:
            ECLWarp(0x0edc);
            ecl << pair{0x1274, (int16_t)0x0};
            ecl << pair{0x12f0, (int16_t)0x0};
            ecl << pair{0x1018, 0x0};
            ECLSetHealth(ecl, 0x10dc, 0x1e, 0x513);
            ECLStall(ecl, 0x10ec);
            break;
        case THPrac::TH06::TH06_ST3_BOSS1:
            if (thPracParam.dlg)
                ECLWarp(0x16d4);
            else
                s3b_n1();
            break;
        case THPrac::TH06::TH06_ST3_BOSS2:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ECLSetTime(ecl, 0x2160, 0x0, 0x0);
            ECLStall(ecl, 0x2170);
            break;
        case THPrac::TH06::TH06_ST3_BOSS3:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ecl << pair{0x2160, 0x0};
            ecl << pair{0x216c, 0x14};
            ecl << pair{0x25f4, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST3_BOSS4:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ecl << pair{0x2160, 0x0};
            ecl << pair{0x216c, 0x14};
            ecl << pair{0x25f4, (int16_t)0x0};
            ECLSetTime(ecl, 0x267c, 0x0, 0x0);
            ECLStall(ecl, 0x268c);
            break;
        case THPrac::TH06::TH06_ST3_BOSS5:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ecl << pair{0x2160, 0x0};
            ecl << pair{0x216c, 0x1a};
            ecl << pair{0x31d0, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST3_BOSS6:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ecl << pair{0x2160, 0x0};
            ecl << pair{0x216c, 0x1a};
            ecl << pair{0x31d0, (int16_t)0x0};
            ECLSetTime(ecl, 0x3254, 0x0, 0x0);
            ECLStall(ecl, 0x3264);
            break;
        case THPrac::TH06::TH06_ST3_BOSS7:
            s3b_n1();
            ecl << pair{0x214c, 0x0};
            ecl << pair{0x2160, 0x0};
            ecl << pair{0x216c, 0x1a};
            ecl << pair{0x31d0, (int16_t)0x0};
            ECLSetTime(ecl, 0x3254, 0x0, 0x0);
            ECLStall(ecl, 0x3264);
            ecl << pair{0x3168, 0x7d0};
            ecl << pair{0x31a8, 0x21};
            ecl << pair{0x31b8, 0x21};
            ecl << pair{0x4b64, (int16_t)0x0};
            ecl << pair{0x4bec, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST4_BOOKS:
            ECLWarp(0x0d40);
            break;
        case THPrac::TH06::TH06_ST4_MID1:
            ECLWarp(0x1024);
            break;
        case THPrac::TH06::TH06_ST4_BOSS1:
            if (thPracParam.dlg)
                ECLWarp(0x29c6);
            else
                ECLWarp(0x29c7);
            break;
        case THPrac::TH06::TH06_ST4_BOSS2:
            ECLWarp(0x29c7);
            s4b_time();
            ECLSetTime(ecl, 0x2810, 0x0, 0x0);
            ECLStall(ecl, 0x2820);
            break;
        case THPrac::TH06::TH06_ST4_BOSS3:
            ECLWarp(0x29c7);
            s4b_time();
            ecl << pair{0x2854, (int16_t)0x23};
            ecl << pair{0x285c, 0x25};
            ecl << pair{0x6da0, (int16_t)0x0};
            ECLSetTime(ecl, 0x7440, 0x0, 0x0);
            ECLStall(ecl, 0x7450);
            break;
        case THPrac::TH06::TH06_ST4_BOSS4:
            ECLWarp(0x29c7);
            s4b_time();
            ecl << pair{0x2854, (int16_t)0x23};
            ecl << pair{0x285c, 0x25};
            ecl << pair{0x6da0, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST4_BOSS5:
            ECLWarp(0x29c7);
            s4b_time();
            ecl << pair{0x2854, (int16_t)0x23};
            ecl << pair{0x285c, 0x27};
            ecl << pair{0x7950, (int16_t)0x0};
            ecl << pair{0x7afc, 0x0};
            ecl << pair{0x7b0c, 0x0};
            ecl << pair{0x7b2c, 0x0};
            ecl << pair{0x7b4c, 0x0};
            ecl << pair{0x7b6c, 0x0};
            ecl << pair{0x7b8c, 0x0};
            break;
        case THPrac::TH06::TH06_ST4_BOSS6:
            ECLWarp(0x29c7);
            s4b_time();
            ecl << pair{0x2854, (int16_t)0x23};
            ecl << pair{0x285c, 0x27};
            ecl << pair{0x7950, (int16_t)0x0};

            ECLSetHealth(ecl, 0x7afc, 0, 1699);
            ECLSetHealth(ecl, 0x7b0c, 0, 3399);
            ECLStall(ecl, 0x7b1c);
            ecl << pair{0x7b04, (int16_t)0x0200} << pair{0x7b14, (int16_t)0x0c00};
            ecl << pair{0x7bc8, (int16_t)0}
                << pair{0x7cf4, (int16_t)0} << pair{0x7d0c, (int16_t)0};
            ecl << pair{0x7d18, 0x0} << pair{0x7d28, 0x0}
                << pair{0x7d48, 0x0} << pair{0x7d68, 0x0}
                << pair{0x7d88, 0x0} << pair{0x7da8, 0x0};
            break;
        case THPrac::TH06::TH06_ST4_BOSS7:
            ECLWarp(0x29c7);
            s4b_time();
            ecl << pair{0x2854, (int16_t)0x23};
            ecl << pair{0x285c, 0x27};
            ecl << pair{0x7950, (int16_t)0x0};

            ECLSetHealth(ecl, 0x7afc, 0, 1699);
            ECLStall(ecl, 0x7b0c);
            ecl << pair{0x7a74, (int16_t)41} << pair{0x7a94, (int16_t)41};
            ecl << pair{0x7a54, (int16_t)1700} << pair{0x7a64, (int16_t)1700};
            ecl << pair{0x7de4, (int16_t)0}
                << pair{0x7ed0, (int16_t)0} << pair{0x7ee8, (int16_t)0};
            ecl << pair{0x7ef4, 0x0} << pair{0x7f04, 0x0}
                << pair{0x7f24, 0x0} << pair{0x7f44, 0x0}
                << pair{0x7f64, 0x0} << pair{0x7f84, 0x0};
            break;
        case THPrac::TH06::TH06_ST5_MID1:
            ECLWarp(0x0d2c);
            if (!thPracParam.dlg)
                ecl << pair{0x64a8, (uint16_t)13};
            break;
        case THPrac::TH06::TH06_ST5_MID2:
            ECLWarp(0x0d2c);
            ecl << pair{0x64a4, (int16_t)0x0};
            ECLSetHealth(ecl, 0x14d8, 0x1e, 0x2c5);
            ECLStall(ecl, 0x14e8);
            break;
        case THPrac::TH06::TH06_ST5_BOSS1:
            ECLWarp(0x1e18);
            if (!thPracParam.dlg) {
                ecl << pair{0x767c, (int16_t)0x0};
                ecl << pair{0x22c8, 0x0};
                ecl << pair{0x22e8, 0x0};
                ecl << pair{0x2308, 0x0};
                ecl << pair{0x2328, 0x0};
                ecl << pair{0x2348, 0x0};
                ecl << pair{0x2218, (int16_t)0x0};
            }
            break;
        case THPrac::TH06::TH06_ST5_BOSS2:
            ECLWarp(0x1e18);
            ecl << pair{0x767c, (int16_t)0x0};
            ecl << pair{0x22c8, 0x0};
            ecl << pair{0x22e8, 0x0};
            ecl << pair{0x2308, 0x0};
            ecl << pair{0x2328, 0x0};
            ecl << pair{0x2348, 0x0};
            ecl << pair{0x2218, (int16_t)0x0};
            ECLSetTime(ecl, 0x2348, 0x0, 0x0);
            ECLStall(ecl, 0x2358);
            break;
        case THPrac::TH06::TH06_ST5_BOSS3:
            ECLWarp(0x1e18);
            ecl << pair{0x767c, (int16_t)0x0};
            ecl << pair{0x22c8, 0x0};
            ecl << pair{0x22e8, 0x0};
            ecl << pair{0x2308, 0x0};
            ecl << pair{0x2328, 0x0};
            ecl << pair{0x2348, 0x0};
            ecl << pair{0x2218, (int16_t)0x0};
            ecl << pair{0x235c, 0x0};
            ecl << pair{0x2368, 0x22};
            ecl << pair{0x3778, (int16_t)0x0};
            ecl << pair{0x3828, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST5_BOSS4:
            ECLWarp(0x1e18);
            ecl << pair{0x767c, (int16_t)0x0};
            ecl << pair{0x22c8, 0x0};
            ecl << pair{0x22e8, 0x0};
            ecl << pair{0x2308, 0x0};
            ecl << pair{0x2328, 0x0};
            ecl << pair{0x2348, 0x0};
            ecl << pair{0x2218, (int16_t)0x0};
            ecl << pair{0x235c, 0x0};
            ecl << pair{0x2368, 0x22};
            ecl << pair{0x3778, (int16_t)0x0};
            ecl << pair{0x3828, (int16_t)0x0};
            ECLSetTime(ecl, 0x38b8, 0x0, 0x0);
            ECLStall(ecl, 0x38c8);
            break;
        case THPrac::TH06::TH06_ST5_BOSS5:
            ECLWarp(0x1e18);
            ecl << pair{0x767c, (int16_t)0x0};
            ecl << pair{0x22c8, 0x0};
            ecl << pair{0x22e8, 0x0};
            ecl << pair{0x2308, 0x0};
            ecl << pair{0x2328, 0x0};
            ecl << pair{0x2348, 0x0};
            ecl << pair{0x2218, (int16_t)0x0};
            ecl << pair{0x235c, 0x1e};
            ecl << pair{0x2368, 0x29};
            ecl << pair{0x4638, (int16_t)0x0};
            ecl << pair{0x46e8, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST5_BOSS6:
            ECLWarp(0x1e18);
            ecl << pair{0x767c, (int16_t)0x0};
            ecl << pair{0x22c8, 0x0};
            ecl << pair{0x22e8, 0x0};
            ecl << pair{0x2308, 0x0};
            ecl << pair{0x2328, 0x0};
            ecl << pair{0x2348, 0x0};
            ecl << pair{0x2218, (int16_t)0x0};
            ecl << pair{0x235c, 0x1e};
            ecl << pair{0x2368, 0x29};
            ecl << pair{0x4638, (int16_t)0x0};
            ecl << pair{0x46e8, (int16_t)0x0};
            ecl << pair{0x235c, 0x0};
            ECLSetTime(ecl, 0x4758, 0x0, 0x0);
            ECLStall(ecl, 0x4768);
            break;
        case THPrac::TH06::TH06_ST6_MID1:
            ECLWarp(0x0a04);
            if (!thPracParam.dlg) {
                ecl << pair{0x77f2, (int16_t)0x0};
                ecl << pair{0x9e8, 0x1};
            }
            break;
        case THPrac::TH06::TH06_ST6_MID2:
            shot = MenuShot();
            if (shot > 1)
                shot = 1099;
            else if (!shot)
                shot = 749;
            else
                shot = 999;
            ECLWarp(0x0a04);
            ecl << pair{0x77f2, (int16_t)0x0};
            ecl << pair{0x0d2c, 0x0};
            ECLSetHealth(ecl, 0x0d3c, 0x0, shot);
            ECLStall(ecl, 0x0d4c);
            break;
        case THPrac::TH06::TH06_ST6_BOSS1:
            if (thPracParam.dlg)
                ECLWarp(0x0c5f);
            else {
                ECLWarp(0x0c61);
                ECLNameFix();
                ecl << pair{0x1686, 0x0};
                ecl << pair{0x16a6, 0x0};
                ecl << pair{0x16c6, 0x0};
                ecl << pair{0x16e6, 0x0};
                ecl << pair{0x15d6, (int16_t)0x0};
            }
            break;
        case THPrac::TH06::TH06_ST6_BOSS2:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ECLSetTime(ecl, 0x1706, 0x0, 0x0);
            ECLStall(ecl, 0x1716);
            break;
        case THPrac::TH06::TH06_ST6_BOSS3:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x0};
            ecl << pair{0x1726, 0x13};
            ecl << pair{0x1b8e, (int16_t)0x0};
            ecl << pair{0x1c3e, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST6_BOSS4:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x0};
            ecl << pair{0x1726, 0x13};
            ecl << pair{0x1b8e, (int16_t)0x0};
            ecl << pair{0x1c3e, (int16_t)0x0};
            ECLSetTime(ecl, 0x1cf2, 0x0, 0x0);
            ECLStall(ecl, 0x1d02);
            break;
        case THPrac::TH06::TH06_ST6_BOSS5:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x1e};
            ecl << pair{0x1726, 0x17};
            ecl << pair{0x28e2, (int16_t)0x0};
            ecl << pair{0x2992, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST6_BOSS6:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x1e};
            ecl << pair{0x1726, 0x17};
            ecl << pair{0x28e2, (int16_t)0x0};
            ecl << pair{0x2992, (int16_t)0x0};
            ecl << pair{0x171a, 0x0};
            ECLSetTime(ecl, 0x2a22, 0x0, 0x0);
            ECLStall(ecl, 0x2a32);
            break;
        case THPrac::TH06::TH06_ST6_BOSS7:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x0};
            ecl << pair{0x1726, 0x1a};
            ecl << pair{0x2d8e, (int16_t)0x0};
            ecl << pair{0x2e3e, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST6_BOSS8:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x0};
            ecl << pair{0x1726, 0x1a};
            ecl << pair{0x2d8e, (int16_t)0x0};
            ecl << pair{0x2e3e, (int16_t)0x0};
            ECLSetTime(ecl, 0x2ee6, 0x0, 0x0);
            ECLStall(ecl, 0x2ef6);
            break;
        case THPrac::TH06::TH06_ST6_BOSS9:
            ECLNameFix();
            ECLWarp(0x0c61);
            ecl << pair{0x1686, 0x0};
            ecl << pair{0x16a6, 0x0};
            ecl << pair{0x16c6, 0x0};
            ecl << pair{0x16e6, 0x0};
            ecl << pair{0x15d6, (int16_t)0x0};
            ecl << pair{0x1706, 0x0};
            ecl << pair{0x171a, 0x0};
            ecl << pair{0x1732, 0x0};
            ecl << pair{0x1726, 0x2b};
            ecl << pair{0x1722, (int16_t)0x0300};
            ecl << pair{0x1736, (int16_t)0x23};
            ecl << pair{0x173a, (int16_t)0x0c00};
            ecl << pair{0x173e, 0x2c};
            ecl << pair{0x5c8e, (int16_t)0x0};
            ecl << pair{0x6290, (int16_t)0x0};
            ecl << pair{0x1622, 0xffffffff};
            break;
        case THPrac::TH06::TH06_ST7_MID1:
            ECLWarp(0x1284);
            if (!thPracParam.dlg)
                ecl << pair{0x0d2e2, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_MID2:
            ECLWarp(0x1284);
            ecl << pair{0x0d2e2, (int16_t)0x0};
            ecl << pair{0x1b14, 0x12};
            ecl << pair{0x1c2c, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_MID3:
            ECLWarp(0x1284);
            ecl << pair{0x0d2e2, (int16_t)0x0};
            ecl << pair{0x1b14, 0x13};
            ecl << pair{0x1d7c, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_NS1:
            if (thPracParam.dlg)
                ECLWarp(0x2191);
            else {
                ECLNameFix();
                s7b_n1();
            }
            break;
        case THPrac::TH06::TH06_ST7_END_S1:
            ECLNameFix();
            s7b_n1();
            ECLSetTime(ecl, 0x344e, 0x0, 0x0);
            break;
        case THPrac::TH06::TH06_ST7_END_NS2:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x23};
            ecl << pair{0x4210, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S2:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x23};
            ecl << pair{0x4210, (int16_t)0x0};
            ecl << pair{0x41fc, 0x0};
            ecl << pair{0x420c, 0x0};
            ECLSetTime(ecl, 0x421c, 0x0, 0x0);
            ECLStall(ecl, 0x422c);
            break;
        case THPrac::TH06::TH06_ST7_END_NS3:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x26};
            ecl << pair{0x4c62, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S3:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x26};
            ecl << pair{0x4c62, (int16_t)0x0};
            ecl << pair{0x4c4e, 0x0};
            ecl << pair{0x4c5e, 0x0};
            ECLSetTime(ecl, 0x4c6e, 0x0, 0x0);
            ECLStall(ecl, 0x4c7e);
            break;
        case THPrac::TH06::TH06_ST7_END_NS4:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x2b};
            ecl << pair{0x59cc, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S4:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x2b};
            ecl << pair{0x59cc, (int16_t)0x0};
            ecl << pair{0x59b8, 0x0};
            ecl << pair{0x59c8, 0x0};
            ECLSetTime(ecl, 0x59d8, 0x0, 0x0);
            ECLStall(ecl, 0x59e8);
            break;
        case THPrac::TH06::TH06_ST7_END_NS5:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x2f};
            ecl << pair{0x63a2, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S5:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x2f};
            ecl << pair{0x63a2, (int16_t)0x0};
            ecl << pair{0x638e, 0x0};
            ecl << pair{0x639e, 0x0};
            ECLSetTime(ecl, 0x63ae, 0x0, 0x0);
            ECLStall(ecl, 0x63be);
            break;
        case THPrac::TH06::TH06_ST7_END_NS6:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x31};
            ecl << pair{0x6b1c, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S6:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x31};
            ecl << pair{0x6b1c, (int16_t)0x0};
            ecl << pair{0x6b08, 0x0};
            ecl << pair{0x6b18, 0x0};
            ECLSetTime(ecl, 0x6b28, 0x0, 0x0);
            ECLStall(ecl, 0x6b38);
            break;
        case THPrac::TH06::TH06_ST7_END_NS7:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x35};
            ecl << pair{0x78aa, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S7:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x35};
            ecl << pair{0x78aa, (int16_t)0x0};
            ecl << pair{0x7896, 0x0};
            ecl << pair{0x78a6, 0x0};
            ECLSetTime(ecl, 0x78b6, 0x0, 0x0);
            ECLStall(ecl, 0x78c6);
            break;
        case THPrac::TH06::TH06_ST7_END_NS8:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x38};
            ecl << pair{0x8508, (int16_t)0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S8:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x38};
            ecl << pair{0x8508, (int16_t)0x0};
            ecl << pair{0x84f4, 0x0};
            ecl << pair{0x8504, 0x0};
            ECLSetTime(ecl, 0x8514, 0x0, 0x0);
            ECLStall(ecl, 0x8524);
            break;
        case THPrac::TH06::TH06_ST7_END_S9:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x3b};
            ecl << pair{0x940a, (int16_t)0x0};
            ecl << pair{0x93f6, 0x0};
            ecl << pair{0x9406, 0x0};
            ecl << pair{0x9416, 0x0};
            ecl << pair{0x9422, 0x0};
            ecl << pair{0x943a, 0x0};
            ecl << pair{0x9466, 0x0};
            ecl << pair{0x9472, 0x0};
            ecl << pair{0x9482, 0x0};
            break;
        case THPrac::TH06::TH06_ST7_END_S10:
            ECLNameFix();
            s7b_n1();
            s7b_call();
            ecl << pair{0x345a, 0x43};
            ecl << pair{0x0bea4, (int16_t)0x0};
            ecl << pair{0x0be90, 0x0};
            ecl << pair{0x0bea0, 0x0};
            ecl << pair{0x0beb0, 0x0};
            ecl << pair{0x0bed0, 0x0};
            ecl << pair{0x0bef0, 0x0};
            ecl << pair{0x0bf10, 0x0};
            ecl << pair{0x0bf30, 0x0};
            ecl << pair{0x0bf50, 0x0};
            ecl << pair{0x0bf5c, 0x0};
            ecl << pair{0x0bf74, 0x0};
            ecl << pair{0x0bfa0, 0x0};
            ecl << pair{0x0bfac, 0x0};
            ecl << pair{0x0bfbc, 0x0};
            break;
        default:
            break;
        }
    }
// Original TH06 stage portions; ECLWarp translates to the NC timeline.
    TH_NOINLINE void THStageWarp([[maybe_unused]] ECLHelper& ecl, int stage, int portion)
    {
        if (stage == 1) {
            switch (portion) {
            case 1:
                ECLWarp(68);
                break;
            case 2:
                ECLWarp(580);
                break;
            case 3:
                ECLWarp(1160);
                break;
            case 4:
                ECLWarp(1540);
                break;
            case 5:
                ECLWarp(2348);
                break;
            case 6:
                ECLWarp(4438);
                break;
            default:
                break;
            }
        } else if (stage == 2) {
            switch (portion) {
            case 1:
                ECLWarp(270);
                break;
            case 2:
                ECLWarp(924);
                break;
            case 3:
                ECLWarp(3528);
                break;
            case 4:
                ECLWarp(4563);
                break;
            default:
                break;
            }
        } else if (stage == 3) {
            switch (portion) {
            case 1:
                ECLWarp(340);
                break;
            case 2:
                ECLWarp(1050);
                break;
            case 3:
                ECLWarp(1670);
                break;
            case 4:
                ECLWarp(2762);
                break;
            case 5:
                ECLWarp(3807);
                break;
            case 6:
                ECLWarp(4118);
                break;
            case 7:
                ECLWarp(5274);
                break;
            default:
                break;
            }
        } else if (stage == 4) {
            switch (portion) {
            case 1:
                ECLWarp(380);
                break;
            case 2:
                ECLWarp(1454);
                break;
            case 3:
                ECLWarp(2328);
                break;
            case 4:
                ECLWarp(0x0d40);
                break;
            case 5:
                ECLWarp(4872);
                break;
            case 6:
                ECLWarp(5712);
                break;
            case 7:
                ECLWarp(7434);
                break;
            case 8:
                ECLWarp(8354);
                break;
            case 9:
                ECLWarp(9784);
                break;
            default:
                break;
            }
        } else if (stage == 5) {
            switch (portion) {
            case 1:
                ECLWarp(350);
                break;
            case 2:
                ECLWarp(1352);
                break;
            case 3:
                ECLWarp(2292);
                break;
            case 4:
                ECLWarp(3814);
                break;
            case 5:
                ECLWarp(6774);
                break;
            default:
                break;
            }
        } else if (stage == 6) {
            switch (portion) {
            case 1:
                ECLWarp(380);
                break;
            case 2:
                ECLWarp(1484);
                break;
            default:
                break;
            }
        } else if (stage == 7) {
            switch (portion) {
            case 1:
                ECLWarp(380);
                break;
            case 2:
                ECLWarp(1300);
                break;
            case 3:
                ECLWarp(2600);
                break;
            case 4:
                ECLWarp(3680);
                break;
            case 5:
                ECLWarp(4803);
                break;
            case 6:
                ECLWarp(5933);
                break;
            case 7:
                ECLWarp(7733);
                break;
            default:
                break;
            }
        }
    }
}
bool ApplyEcl() {
    failureOffset=0;
    needsBossAssets=false;
    warpedFrame=thPracParam.frame;
    ECLHelper ecl;
    try {
        if(thPracParam.stage<0||thPracParam.stage>6)return false;
        auto script=*reinterpret_cast<const unsigned char**>(imageBase+Rva::EclFile);
        auto guard=scriptGuards[thPracParam.stage];
        if(!script)return false;
        MEMORY_BASIC_INFORMATION memory{};
        if(!VirtualQuery(script,&memory,sizeof(memory))||memory.State!=MEM_COMMIT||
           script+guard.size>static_cast<unsigned char*>(memory.BaseAddress)+memory.RegionSize)return false;
        uint64_t hash=14695981039346656037ULL;
        for(size_t i=0;i<guard.size;++i)hash=(hash^script[i])*1099511628211ULL;
        if(hash!=guard.digest)return false;
        if(thPracParam.section>=10000) {
            int portion=thPracParam.section%100;
            if(portion<1||portion>PortionCounts[thPracParam.stage]||thPracParam.section/100!=101+thPracParam.stage)return false;
            THStageWarp(ecl,thPracParam.stage+1,portion);
        } else if(IsAddedSpell(thPracParam.section)){
            if(thPracParam.stage!=6)return false;
            ECLNameFix();
            // Use the extra boss's own timeline, beyond the original final
            // card and its unlock/dialogue branch. Preserve all later phases.
            warpedFrame=8500;
            constexpr int entries[]={101,106,112};
            ecl.Native(0x12f24,0,2,true,int16_t(37)); // Spawn the normal Flandre base.
            ecl.Native(0x3436,109,12,false,entries[thPracParam.section-FragileWing]);
        } else if(thPracParam.section)THPatch(ecl,static_cast<th_sections_t>(thPracParam.section));
        if(!ecl.Apply())return false;
        *reinterpret_cast<int*>(imageBase+Rva::TimelineFrame)=warpedFrame;
        return true;
    } catch(...) {return false;}
}
int EclFailureOffset() { return failureOffset; }
bool EclNeedsBossAssets() { return needsBossAssets; }
int NativeTimelineFrame(int stage,int oldTime) {
    for(auto& map:frameMap)if(map.stage==stage && map.oldTime>=oldTime)return map.newTime+(oldTime-map.oldTime);
    return oldTime;
}
}
// Spell names (th06nc/spell_names.h)
namespace THPrac::TH06NC {
// Steam th06nc 1.03 localization, ST_ECLDATA1..7 (64 original spell names).
// Match the upstream Japanese label; only NC English/Japanese menus use these.
struct SpellName { const char* originalJapanese; th_glossary_t localized; };
inline constexpr SpellName SpellNames[]={
    {"火符「アグニシャイン」",TH06NC_ECLDATA4_SUB42_0},
    {"火符「アグニシャイン上級」",TH06NC_ECLDATA4_SUB43_0},
    {"火符「アグニレイディアンス」",TH06NC_ECLDATA4_SUB44_0},
    {"木符「シルフィホルン」",TH06NC_ECLDATA4_SUB47_0},
    {"木符「シルフィホルン上級」",TH06NC_ECLDATA4_SUB48_0},
    {"木符「グリーンストーム」",TH06NC_ECLDATA4_SUB49_0},
    {"土符「レイジィトリリトン」",TH06NC_ECLDATA4_SUB50_0},
    {"土符「レイジィトリリトン上級」",TH06NC_ECLDATA4_SUB51_0},
    {"土符「トリリトンシェイク」",TH06NC_ECLDATA4_SUB52_0},
    {"水符「プリンセスウンディネ」",TH06NC_ECLDATA4_SUB45_0},
    {"水符「ベリーインレイク」",TH06NC_ECLDATA4_SUB46_0},
    {"金符「メタルファティーグ」",TH06NC_ECLDATA4_SUB53_0},
    {"金符「シルバードラゴン」",TH06NC_ECLDATA4_SUB54_0},
    {"火＆土符「ラーヴァクロムレク」",TH06NC_ECLDATA4_SUB55_0},
    {"水＆木符「ウォーターエルフ」",TH06NC_ECLDATA4_SUB57_0},
    {"木＆火符「フォレストブレイズ」",TH06NC_ECLDATA4_SUB56_0},
    {"土＆金符「エメラルドメガリス」",TH06NC_ECLDATA4_SUB59_0},
    {"金＆水符「マーキュリポイズン」",TH06NC_ECLDATA4_SUB58_0},
    {"月符「ムーンライトレイ」",TH06NC_ECLDATA1_SUB9_0},
    {"夜符「ナイトバード」",TH06NC_ECLDATA1_SUB22_0},
    {"闇符「ディマーケイション」",TH06NC_ECLDATA1_SUB23_0},
    {"氷符「アイシクルフォール」",TH06NC_ECLDATA2_SUB29_0},
    {"雹符「ヘイルストーム」",TH06NC_ECLDATA2_SUB30_0},
    {"凍符「パーフェクトフリーズ」",TH06NC_ECLDATA2_SUB31_0},
    {"雪符「ダイアモンドブリザード」",TH06NC_ECLDATA2_SUB32_0},
    {"華符「芳華絢爛」",TH06NC_ECLDATA3_SUB13_0},
    {"華符「セラギネラ９」",TH06NC_ECLDATA3_SUB14_0},
    {"虹符「彩虹の風鈴」",TH06NC_ECLDATA3_SUB29_0},
    {"幻符「華想夢葛」",TH06NC_ECLDATA3_SUB30_0},
    {"彩符「彩雨」",TH06NC_ECLDATA3_SUB31_0},
    {"彩符「彩光乱舞」",TH06NC_ECLDATA3_SUB32_0},
    {"彩符「極彩颱風」",TH06NC_ECLDATA3_SUB33_0},
    {"奇術「ミスディレクション」",TH06NC_ECLDATA5_SUB18_0},
    {"奇術「幻惑ミスディレクション」",TH06NC_ECLDATA5_SUB18_1},
    {"幻在「クロックコープス」",TH06NC_ECLDATA5_SUB44_0},
    {"幻幽「ジャック・ザ・ルドビレ」",TH06NC_ECLDATA5_SUB45_0},
    {"幻象「ルナクロック」",TH06NC_ECLDATA5_SUB46_0},
    {"幻世「ザ・ワールド」",TH06NC_ECLDATA5_SUB47_0},
    {"メイド秘技「操りドール」",TH06NC_ECLDATA5_SUB50_0},
    {"メイド秘技「殺人ドール」",TH06NC_ECLDATA5_SUB50_1},
    {"奇術「エターナルミーク」",TH06NC_ECLDATA6_SUB14_0},
    {"天罰「スターオブダビデ」",TH06NC_ECLDATA6_SUB30_0},
    {"神罰「幼きデーモンロード」",TH06NC_ECLDATA6_SUB31_0},
    {"冥符「紅色の冥界」",TH06NC_ECLDATA6_SUB32_0},
    {"獄符「千本の針の山」",TH06NC_ECLDATA6_SUB33_0},
    {"呪詛「ブラド・ツェペシュの呪い」",TH06NC_ECLDATA6_SUB34_0},
    {"神術「吸血鬼幻想」",TH06NC_ECLDATA6_SUB35_0},
    {"紅符「スカーレットシュート」",TH06NC_ECLDATA6_SUB38_0},
    {"紅符「スカーレットマイスタ」",TH06NC_ECLDATA6_SUB39_0},
    {"「レッドマジック」",TH06NC_ECLDATA6_SUB43_0},
    {"「紅色の幻想郷」",TH06NC_ECLDATA6_SUB44_0},
    {"月符「サイレントセレナ」",TH06NC_ECLDATA7_SUB22_0},
    {"日符「ロイヤルフレア」",TH06NC_ECLDATA7_SUB23_0},
    {"火水木金土符「賢者の石」",TH06NC_ECLDATA7_SUB24_0},
    {"禁忌「クランベリートラップ」",TH06NC_ECLDATA7_SUB33_0},
    {"禁忌「レーヴァテイン」",TH06NC_ECLDATA7_SUB36_0},
    {"禁忌「フォーオブアカインド」",TH06NC_ECLDATA7_SUB39_0},
    {"禁忌「カゴメカゴメ」",TH06NC_ECLDATA7_SUB44_0},
    {"禁忌「恋の迷路」",TH06NC_ECLDATA7_SUB48_0},
    {"禁弾「スターボウブレイク」",TH06NC_ECLDATA7_SUB50_0},
    {"禁弾「カタディオプトリック」",TH06NC_ECLDATA7_SUB54_0},
    {"禁弾「過去を刻む時計」",TH06NC_ECLDATA7_SUB57_0},
    {"秘弾「そして誰もいなくなるか？」",TH06NC_ECLDATA7_SUB60_0},
    {"QED「495年の波紋」",TH06NC_ECLDATA7_SUB68_0},
};
}

// Section menu (th06nc/section_menu.h)

namespace THPrac::TH06NC {
// Runtime IDs 71-73 are already used in NC replays. The upstream name table
// uses those indices for stage-four shot variants, so UI labels need their own IDs.
inline constexpr int OriginalSectionNameCount = int(std::size(TH06::th_sections_str[0][0]));
inline constexpr int SectionNameCount = OriginalSectionNameCount + 3;

struct SectionMenu {
    std::array<int, std::size(TH06::th_sections_cba[0][0]) + 3> values{};
    std::array<int, std::size(TH06::th_sections_cba[0][0]) + 3> labels{};
    int count = 0;

    void Normalize(int& selection, const char* const* names) const {
        // Keep the selection as close as possible instead of jumping back to
        // the first entry (e.g. the first spell card) when the stage changed.
        if (selection < 0) selection = 0;
        else if (selection >= count && count > 0) selection = count - 1;
        for (int n = 0; n < count; ++n) {
            const auto* name = names[labels[selection]];
            if (name && *name) return;
            selection = (selection + 1) % count;
        }
    }
};

inline SectionMenu BuildSectionMenu(int stage, int warp, int shot, int fakeShot) {
    SectionMenu menu;
    if (stage < 0 || stage >= 7 || warp < 2 || warp > 5) return menu;
    const int labelStage = stage == 3 ? 7 + std::clamp(fakeShot ? fakeShot - 1 : shot, 0, 3) : stage;
    const auto append = [&](const auto& values, const auto& labels) {
        static_assert(std::extent_v<std::remove_reference_t<decltype(values)>> < std::tuple_size_v<decltype(menu.values)>);
        for (size_t i = 0; i < std::size(values) && values[i]; ++i) {
            menu.values[menu.count] = values[i];
            menu.labels[menu.count++] = labels[i];
        }
    };
    if (warp <= 3) append(TH06::th_sections_cba[stage][warp - 2], TH06::th_sections_cba[labelStage][warp - 2]);
    else append(TH06::th_sections_cbt[stage][warp - 4], TH06::th_sections_cbt[labelStage][warp - 4]);
    if (stage == 6 && (warp == 3 || warp == 5)) {
        for (int i = 0; i < 3; ++i) {
            menu.values[menu.count] = FragileWing + i;
            menu.labels[menu.count++] = OriginalSectionNameCount + i;
        }
    }
    return menu;
}
}

// ---- Overlay (th06nc/overlay.cpp) ----

namespace THPrac {
constinit THPracSettings gSettings{};
constinit HotkeyChords hotkeys{};
}
namespace THPrac::TH06NC {
using namespace TH06;
THPracParam thPracParam{};
const char** SectionNames(int difficulty) {
    static std::array<const char*,SectionNameCount> names{};
    const th_glossary_t added[]={TH06NC_FRAGILE_WING,TH06NC_CREEPING_BLOODSTAIN,TH06NC_DANMAKU_HEART};
    auto locale=Gui::LocaleGet();
    for(int i=0;i<OriginalSectionNameCount;++i) {
        names[i]=th_sections_str[locale][difficulty][i];
        if(locale!=LOCALE_ZH_CN) {
            const auto* original=th_sections_str[LOCALE_JA_JP][difficulty][i];
            for(const auto& spell:SpellNames) {
                if(original&&strcmp(original,spell.originalJapanese)==0){names[i]=S(spell.localized);break;}
            }
        }
    }
    for(int i=0;i<3;++i)names[OriginalSectionNameCount+i]=S(added[i]);
    return names.data();
}
// Adapted from TH06::THGuiPrac in thprac_th06.cpp. Uses the same controls and selectors.
    class THGuiPrac : public Gui::GameGuiWnd {
    public:
        THGuiPrac() noexcept
        {
            *mLife = 8;
            *mBomb = 8;
            *mPower = 128;
            *mMode = 1;
            *mScore = 0;
            *mGraze = 0;
            *mRank = 32;

            SetFade(0.8f, 0.1f);
            SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
            SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);
            OnLocaleChange();
            LoadPersisted();
        }

    public:

        // Practice selections survive menu reopens and game restarts. The
        // values are written when a run is confirmed and restored on startup.
        struct Persisted {
            uint32_t magic;
            uint32_t version;
            int32_t mode, stage, warp, chapter, phase, frame, section;
            int32_t dlg, customRank, fakeShot;
            int32_t life, bomb, power, graze, point, rank;
            int64_t score;
            int32_t difficulty, shotType;
        };
        static constexpr uint32_t kPersistMagic = 0x53504336;
        static constexpr uint32_t kPersistVersion = 3;

        void SavePersisted()
        {
            Persisted p {};
            p.magic = kPersistMagic;
            p.version = kPersistVersion;
            p.mode = *mMode;
            p.stage = *mStage;
            p.warp = *mWarp;
            p.chapter = *mChapter;
            p.phase = *mPhase;
            p.frame = *mFrame;
            p.section = *mSection;
            p.dlg = *mDlg;
            p.customRank = *mCustomRank;
            p.fakeShot = *mFakeShot;
            p.life = *mLife;
            p.bomb = *mBomb;
            p.power = *mPower;
            p.graze = *mGraze;
            p.point = *mPoint;
            p.rank = *mRank;
            p.score = *mScore;
            p.difficulty = mDiffculty;
            p.shotType = mShotType;
            FILE* f = fopen("thprac-linux.prac", "wb");
            if (f) {
                fwrite(&p, sizeof(p), 1, f);
                fclose(f);
            }
        }

        void LoadPersisted()
        {
            FILE* f = fopen("thprac-linux.prac", "rb");
            if (!f) return;
            Persisted p {};
            bool ok = fread(&p, sizeof(p), 1, f) == 1;
            fclose(f);
            if (!ok || p.magic != kPersistMagic || p.version != kPersistVersion) return;
            *mMode = p.mode;
            *mStage = p.stage;
            *mWarp = p.warp;
            *mPhase = p.phase;
            *mFrame = p.frame;
            *mSection = p.section;
            *mDlg = p.dlg != 0;
            *mCustomRank = p.customRank != 0;
            *mFakeShot = p.fakeShot;
            *mLife = p.life;
            *mBomb = p.bomb;
            *mPower = p.power;
            *mGraze = p.graze;
            *mPoint = p.point;
            *mRank = p.rank;
            *mScore = p.score;
            mDiffculty = p.difficulty;
            mShotType = p.shotType;
            if (p.stage >= 0 && p.stage < 7) {
                int maxChapter = mChapterSetup[p.stage][0] + mChapterSetup[p.stage][1];
                *mChapter = p.chapter < 1 ? 1 : (p.chapter > maxChapter ? maxChapter : p.chapter);
            } else {
                *mChapter = 1;
            }
        }

        TH_NOINLINE void State(int state)
        {
            switch (state) {
            case 0:
                break;
            case 1:
                SetFade(0.8f, 0.1f);
                Open();
                // Keep every previous selection, including the stage: the GUI's
                // stage combo is what the module applies on start (the game's
                // own menu pick is overridden in GameInit), so nothing is
                // forced here and reopens show exactly the last confirmed set.
                mDiffculty = std::clamp(MenuDifficulty(),0,3);
                mShotType = MenuShot();
                break;
            case 2:
                break;
            case 3:
                SetFade(0.8f, 0.8f);
                Close();
                *mNavFocus = 0;

                // Fill Param
                thPracParam.mode = *mMode;

                thPracParam.stage = *mStage;
                thPracParam.section = CalcSection();
                thPracParam.phase = *mPhase;
                thPracParam.frame = *mFrame;
                thPracParam.dlg = false;
                thPracParam.fakeType = 0;
                if (SectionHasDlg(thPracParam.section))
                    thPracParam.dlg = *mDlg;

                thPracParam.score = *mScore;
                thPracParam.life = (float)*mLife;
                thPracParam.bomb = (float)*mBomb;
                thPracParam.power = (float)*mPower;
                thPracParam.graze = *mGraze;
                thPracParam.point = *mPoint;

                thPracParam.rank = *mCustomRank ? *mRank : NativeRank(*mStage == 6 ? 4 : mDiffculty);
                thPracParam.customRank = *mCustomRank;
                if (thPracParam.section >= TH06_ST4_BOSS1 && thPracParam.section <= TH06_ST4_BOSS7)
                    thPracParam.fakeType = *mFakeShot;
                SavePersisted();
                break;
            case 4:
                Close();
                *mNavFocus = 0;
                break;
            case 5:
                // Fill Param
                thPracParam.mode = *mMode;

                thPracParam.stage = *mStage;
                thPracParam.section = CalcSection();
                thPracParam.phase = *mPhase;
                thPracParam.frame = *mFrame;
                thPracParam.dlg = false;
                thPracParam.fakeType = 0;
                if (SectionHasDlg(thPracParam.section))
                    thPracParam.dlg = *mDlg;

                thPracParam.score = *mScore;
                thPracParam.life = (float)*mLife;
                thPracParam.bomb = (float)*mBomb;
                thPracParam.power = (float)*mPower;
                thPracParam.graze = *mGraze;
                thPracParam.point = *mPoint;

                thPracParam.rank = *mCustomRank ? *mRank : NativeRank(*mStage == 6 ? 4 : mDiffculty);
                thPracParam.customRank = *mCustomRank;
                if (thPracParam.section >= TH06_ST4_BOSS1 && thPracParam.section <= TH06_ST4_BOSS7)
                    thPracParam.fakeType = *mFakeShot;
                break;
            default:
                break;
            }
        }

        void SpellPhase()
        {
            auto section = CalcSection();
            if (section == TH06_ST7_END_S10) {
                mPhase(TH_PHASE, TH_SPELL_PHASE1);
            }
        }
        void PracticeMenu(Gui::GuiNavFocus& nav_focus)
        {
            mMode();
            if (mStage())
                *mSection = *mChapter = *mPhase = *mFrame = 0;
            if (*mMode == 1) {
                if (mWarp())
                    *mSection = *mChapter = *mPhase = *mFrame = 0;
                if (*mWarp) {
                    if (*mStage == 3) {
                        if (mFakeShot()) *mSection = *mPhase = 0;
                    }

                    SectionWidget();
                    SpellPhase();
                }

                mLife();
                mBomb();
                mScore();
                mScore.RoundDown(10);
                mPower();
                mGraze();
                mPoint();
                if (!*mCustomRank) {
                    *mRank = NativeRank(*mStage == 6 ? 4 : mDiffculty);
                    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                }
                mRank();
                if (!*mCustomRank) {
                    ImGui::PopStyleVar();
                    ImGui::PopItemFlag();
                }
                mCustomRank();
            }

            nav_focus();
        }

    protected:
        virtual void OnLocaleChange() override
        {
            SetTitle(S(TH_MENU));
            switch (Gui::LocaleGet()) {
            case LOCALE_ZH_CN:
                SetSize(330.f, 390.f);
                SetPos(260.f, 65.f);
                SetItemWidth(-60.0f);
                break;
            case LOCALE_EN_US:
                SetSize(370.f, 375.f);
                SetPos(240.f, 75.f);
                SetItemWidth(-60.0f);
                break;
            case LOCALE_JA_JP:
                SetSize(330.f, 390.f);
                SetPos(260.f, 65.f);
                SetItemWidth(-65.0f);
                break;
            default:
                break;
            }
        }
        virtual void OnContentUpdate() override
        {
            ImGui::TextUnformatted(S(TH_MENU));
            ImGui::Separator();

            PracticeMenu(mNavFocus);
        }
        int CalcSection()
        {
            int chapterId = 0;
            switch (*mWarp) {
            case 1: // Chapter
                // Chapter Id = 10000 + Stage * 100 + Section
                chapterId += (*mStage + 1) * 100;
                chapterId += *mChapter;
                chapterId += 10000; // Base of chapter ID is 1000.
                return chapterId;
                break;
            case 2:
            case 3: // Mid boss & End boss
            case 4:
            case 5: { // Non-spell & Spellcard
                auto menu = BuildSectionMenu(*mStage, *mWarp, mShotType, *mFakeShot);
                menu.Normalize(*mSection, SectionNames(mDiffculty));
                return menu.values[*mSection];
            }
            default:
                return 0;
                break;
            }
        }
        bool SectionHasDlg(int32_t section)
        {
            switch (section) {
            case TH06_ST1_BOSS1:
            case TH06_ST2_BOSS1:
            case TH06_ST3_BOSS1:
            case TH06_ST4_BOSS1:
            case TH06_ST5_BOSS1:
            case TH06_ST5_MID1:
            case TH06_ST6_BOSS1:
            case TH06_ST6_MID1:
            case TH06_ST7_END_NS1:
            case TH06_ST7_MID1:
                return true;
            default:
                return false;
            }
        }
        void SectionWidget()
        {
            static char chapterStr[256] {};
            auto& chapterCounts = mChapterSetup[*mStage];

            switch (*mWarp) {
            case 1: // Chapter
                mChapter.SetBound(1, chapterCounts[0] + chapterCounts[1]);

                if (chapterCounts[1] == 0) {
                    snprintf(chapterStr, sizeof(chapterStr), S(TH_STAGE_PORTION_N), *mChapter);
                } else if (*mChapter <= chapterCounts[0]) {
                    snprintf(chapterStr, sizeof(chapterStr), S(TH_STAGE_PORTION_1), *mChapter);
                } else {
                    snprintf(chapterStr, sizeof(chapterStr), S(TH_STAGE_PORTION_2), *mChapter - chapterCounts[0]);
                };

                mChapter(chapterStr);
                break;
            case 2:
            case 3: // Mid boss & End boss
            case 4:
            case 5: { // Non-spell & Spellcard
                auto menu = BuildSectionMenu(*mStage, *mWarp, mShotType, *mFakeShot);
                auto names = SectionNames(mDiffculty);
                menu.Normalize(*mSection, names);
                if (mSection(TH_WARP_SELECT_FRAME[*mWarp], menu.labels.data(), names))
                    *mPhase = 0;
                if (SectionHasDlg(menu.values[*mSection]))
                    mDlg();
                break;
            }
            case 6:
                mFrame();
                break;
            }
        }


        // Data
        Gui::GuiCombo mMode { TH_MODE, TH_MODE_SELECT };
        Gui::GuiCombo mStage { TH_STAGE, TH_STAGE_SELECT };
        Gui::GuiCombo mWarp { TH_WARP, TH_WARP_SELECT_FRAME };
        Gui::GuiCombo mSection { TH_MODE };
        Gui::GuiCombo mPhase { TH_PHASE };
        Gui::GuiCheckBox mDlg { TH_DLG };

        Gui::GuiSlider<int, ImGuiDataType_S32> mChapter { TH_CHAPTER, 0, 0 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mFrame { TH_FRAME, 0, 32766 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mLife { TH_LIFE, 0, 8 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mBomb { TH_BOMB, 0, 8 };
        Gui::GuiDrag<int64_t, ImGuiDataType_S64> mScore { TH_SCORE, 0, 9999999990, 10, 100000000 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mPower { TH_POWER, 0, 128 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mGraze { TH_GRAZE, 0, 99999, 1, 10000 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mPoint { TH_POINT, 0, 9999, 1, 1000 };

        Gui::GuiSlider<int, ImGuiDataType_S32> mRank { TH06_RANK, 0, 99, 1, 10, 10 };
        Gui::GuiCheckBox mCustomRank { TH06NC_CUSTOM_RANK };
        Gui::GuiCombo mFakeShot { TH06_FS, TH06_TYPE_SELECT };

        Gui::GuiNavFocus mNavFocus { TH_STAGE, TH_MODE, TH_WARP, TH_FRAME,
            TH_MID_STAGE, TH_END_STAGE, TH_NONSPELL, TH_SPELL, TH_PHASE, TH_CHAPTER,
            TH_LIFE, TH_BOMB, TH_SCORE, TH_POWER, TH_GRAZE, TH_POINT,
            TH06_RANK, TH06NC_CUSTOM_RANK, TH06_FS };

        int mChapterSetup[7][2] {
            { 4, 2 },
            { 2, 2 },
            { 4, 3 },
            { 4, 5 },
            { 3, 2 },
            { 2, 0 },
            { 4, 3 }
        };

        float mStep = 10.0;
        int mDiffculty = 0;
        int mShotType = 0;
    };
THGuiPrac& PracticeUi();
// Adapted from the existing TH06 pause menu.
    class THPauseMenu : public Gui::GameGuiWnd {
    public:
        THPauseMenu() noexcept
        {
            SetTitle("Pause Menu");
            SetFade(0.8f, 0.1f);
            SetSize(384.f, 448.f);
            SetPos(32.f, 16.f);
            SetItemWidth(-60);
            SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
            SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);
        }

    public:

        bool el_bgm_signal { false };
        bool el_bgm_changed { false };

        enum state {
            STATE_CLOSE = 0,
            STATE_RESUME = 1,
            STATE_EXIT = 2,
            STATE_RESTART = 3,
            STATE_OPEN = 4,
        };
        enum signal {
            SIGNAL_NONE = 0,
            SIGNAL_RESUME = 1,
            SIGNAL_EXIT = 2,
            SIGNAL_RESTART = 3,
        };
        signal PMState()
        {
            switch (mState) {
            case THPauseMenu::STATE_CLOSE:
                return StateClose();
            case THPauseMenu::STATE_RESUME:
                return StateResume();
            case THPauseMenu::STATE_EXIT:
                return StateExit();
            case THPauseMenu::STATE_RESTART:
                return StateRestart();
            case THPauseMenu::STATE_OPEN:
                return StateOpen();
            default:
                return SIGNAL_NONE;
            }
        }

    protected:
        signal StateRestart()
        {
            if (mState != STATE_RESTART) {
                mState = STATE_RESTART;
                mFrameCounter = 0;
            }

            if (mFrameCounter == 1) {
                *mNavFocus = 0;
                inSettings = false;

                PracticeUi().State(5);

                Close();
            } else if (mFrameCounter == 10) {
                StateClose();
                return SIGNAL_RESTART;
            }

            return SIGNAL_NONE;
        }
        signal StateExit()
        {
            if (mState != STATE_EXIT) {
                mState = STATE_EXIT;
                mFrameCounter = 0;
            }

            if (mFrameCounter == 1) {
                *mNavFocus = 0;
                inSettings = false;
                Close();
            } else if (mFrameCounter == 10) {
                StateClose();
                return SIGNAL_EXIT;
            }

            return SIGNAL_NONE;
        }
        signal StateResume()
        {
            if (mState != STATE_RESUME) {
                mState = STATE_RESUME;
                mFrameCounter = 0;
            }

            if (mFrameCounter == 1) {
                *mNavFocus = 0;
                inSettings = false;
                Close();
            } else if (mFrameCounter == 10) {
                StateClose();
                return SIGNAL_RESUME;
            }

            return SIGNAL_NONE;
        }
        signal StateClose()
        {
            if (mState != STATE_CLOSE) {
                mState = STATE_CLOSE;
                mFrameCounter = 0;
            }

            if (mFrameCounter > 5) {
                return StateOpen();
            }

            return SIGNAL_NONE;
        }
        signal StateOpen()
        {
            if (mState != STATE_OPEN) {
                mState = STATE_OPEN;
                mFrameCounter = 0;
            }

            if (mFrameCounter == 1) {
                Open();
            }
            if (mFrameCounter > 10) {
                if (Gui::KeyboardInputGetSingle(VK_ESCAPE))
                    StateResume();
                else if (Gui::KeyboardInputGetRaw(0x51))
                    StateExit();
                else if (Gui::KeyboardInputGetRaw(0x52))
                    StateRestart();
            }

            return SIGNAL_NONE;
        }

        virtual void OnPreUpdate() override
        {
            if (mFrameCounter < UINT_MAX)
                mFrameCounter++;
        }
        virtual void OnLocaleChange() override
        {
            switch (Gui::LocaleGet()) {
            case LOCALE_ZH_CN:
                SetItemWidth(-60.0f);
                break;
            case LOCALE_EN_US:
                SetItemWidth(-60.0f);
                break;
            case LOCALE_JA_JP:
                SetItemWidth(-65.0f);
                break;
            default:
                break;
            }
        }
        virtual void OnContentUpdate() override
        {
            if (!inSettings) {
                ImGui::Dummy(ImVec2(10.0f, 140.0f));
                ImGui::Indent(119.0f);
                if (mResume())
                    StateResume();
                ImGui::Spacing();
                if (mExit())
                    StateExit();
                ImGui::Spacing();
                if (mRestart())
                    StateRestart();
                ImGui::Spacing();
                if (mSettings())
                    inSettings = !inSettings;
                ImGui::Spacing();
                ImGui::Unindent();
                mNavFocus();
            } else {
                ImGui::Dummy(ImVec2(10.0f, 10.0f));
                ImGui::Indent(119.0f);
                if (mResume())
                    StateResume();
                ImGui::Spacing();
                if (mExit())
                    StateExit();
                ImGui::Spacing();
                if (mRestart())
                    StateRestart();
                ImGui::Spacing();
                if (mSettings())
                    inSettings = !inSettings;
                ImGui::Spacing();
                ImGui::Unindent(67.0f);
                PracticeUi().PracticeMenu(mNavFocus);
            }
        }

        // Var
        state mState = STATE_CLOSE;
        unsigned int mFrameCounter = 0;
        bool inSettings = false;

        Gui::GuiButton mResume { TH_RESUME, 130.0f, 25.0f };
        Gui::GuiButton mExit { TH_EXIT, 130.0f, 25.0f };
        Gui::GuiButton mRestart { TH_RESTART, 130.0f, 25.0f };
        Gui::GuiButton mSettings { TH_TWEAK, 130.0f, 25.0f };

        Gui::GuiNavFocus mNavFocus { TH_RESUME, TH_EXIT, TH_RESTART, TH_TWEAK,
            TH_STAGE, TH_MODE, TH_WARP,
            TH_MID_STAGE, TH_END_STAGE, TH_NONSPELL, TH_SPELL, TH_PHASE,
            TH_LIFE, TH_BOMB, TH_SCORE, TH_POWER, TH_GRAZE, TH_POINT,
            TH06_RANK, TH06NC_CUSTOM_RANK, TH06_FS };
    };
class THOverlay : public Gui::GameGuiWnd {
public:
    THOverlay() {
        SetTitle("Mod Menu");SetFade(0.5f,0.5f);SetPos(10,10);SetSize(0,0);
        SetWndFlag(ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
            ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoInputs|
            ImGuiWindowFlags_NoFocusOnAppearing);
    }
    void OnContentUpdate() override {
        const th_glossary_t labels[]={TH_MUTEKI,TH_INFLIVES,TH_INFBOMBS,TH_INFPOWER,TH_TIMELOCK,TH_AUTOBOMB,TH_EL_BGM};
        const uint32_t masks[]={1,2,4,8,16,32,128};
        for(int i=0;i<7;++i) {
            if(PracticeFlags()&masks[i])ImGui::TextColored({0,1,0,1},"[F%d: %s]",i+1,S(labels[i]));
            else ImGui::Text("F%d: %s",i+1,S(labels[i]));
        }
    }
};
class THAdvanced : public Gui::GameGuiWnd {
    Gui::GuiSlider<int,ImGuiDataType_S32> fps{"##FPS",30,240};
public:
    THAdvanced(){SetTitle("Advanced Options");SetFade(0.8f,0.8f);SetItemWidth(-1);}
    void OnPreUpdate() override {
        const auto display = ImGui::GetIO().DisplaySize;
        // Fit inside the left margin beside the centered 384-wide pause menu.
        const float width = std::clamp((display.x - 384.0f) * 0.5f - 20.0f, 100.0f, 220.0f);
        SetPos(10.0f, display.y - 120.0f);
        SetSize(width, 110.0f);
    }
    void OnContentUpdate() override {
        ImGui::TextWrapped("%s (F11)", S(TH_GAME_SPEED));
        ImGui::Separator();
        ImGui::TextUnformatted("FPS");
        *fps=PracticeFps();if(fps())SetPracticeFps(*fps);
    }
};
namespace {
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn originalPresent{};
HRESULT PresentFrame(IDXGISwapChain* swap, UINT interval, UINT flags)
{
    return LowLatencyPresent(swap, interval, flags, originalPresent);
}
ID3D11Device* device{};
ID3D11DeviceContext* context{};
ImGuiContext* guiContext{};
std::unique_ptr<THGuiPrac> practice;
std::unique_ptr<THOverlay> overlay;
std::unique_ptr<THAdvanced> advanced;
std::unique_ptr<THPauseMenu> pause;
bool pauseWanted{};
int pauseAction{};
bool wanted{}, opened{};
bool quickWanted{}, advancedWanted{}, inputHooked{};
int action{}, openingFrames{};
uint16_t uiInput{}, uiPrevious{}, uiRepeat{};
DisplayState previousDisplayState=DisplayState::Off;
ULONGLONG displayNoticeUntil{};

bool NumericInputActive()
{
    return (wanted || pauseWanted || advancedWanted) && guiContext &&
        guiContext->ActiveId != 0 && guiContext->ActiveId == guiContext->TempInputId;
}

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swap, UINT interval, UINT flags)
{
    auto displayState=LowLatencyState();
    if(displayState!=previousDisplayState) {
        if(previousDisplayState==DisplayState::Active && displayState==DisplayState::Unavailable)
            displayNoticeUntil=GetTickCount64()+10000;
        previousDisplayState=displayState;
    }
    bool displayNotice=displayState==DisplayState::Unavailable && GetTickCount64()<displayNoticeUntil;
    if(!PracticeActive()){quickWanted=false;advancedWanted=false;pauseWanted=false;}
    if (!wanted && !opened && !quickWanted && !advancedWanted && !pauseWanted && !inputHooked && !displayNotice) return PresentFrame(swap, interval, flags);
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc))) return PresentFrame(swap, interval, flags);
    auto previousContext = ImGui::GetCurrentContext();
    if (!guiContext) {
        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))))
            return PresentFrame(swap, interval, flags);
        device->GetImmediateContext(&context);
        guiContext = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        Gui::LocaleCreateFont(16.0f);
        // Segoe UI lacks the official English spell-name corner quotes.
        // Reuse their Japanese glyphs from the same atlas, retaining each locale's font.
        auto& fonts=ImGui::GetIO().Fonts->Fonts;
        for(ImWchar c:{ImWchar(0xff62),ImWchar(0xff63)}) {
            if(!fonts[LOCALE_EN_US]->FindGlyphNoFallback(c)) {
                if(const auto* glyph=fonts[LOCALE_JA_JP]->FindGlyphNoFallback(c))
                    fonts[LOCALE_EN_US]->AddGlyph(nullptr,c,glyph->X0,glyph->Y0,glyph->X1,glyph->Y1,
                        glyph->U0,glyph->V0,glyph->U1,glyph->V1,glyph->AdvanceX);
            }
        }
        fonts[LOCALE_EN_US]->BuildLookupTable();
        Gui::LocaleSet(static_cast<Locale>(PracticeLanguage()));
        Gui::ImplWin32Init(desc.OutputWindow);
        ImGui_ImplDX11_Init(device, context);
        Gui::InGameInputInit(Gui::INGAGME_INPUT_GEN1,
            reinterpret_cast<uintptr_t>(&uiInput), reinterpret_cast<uintptr_t>(&uiPrevious),
            reinterpret_cast<uintptr_t>(&uiRepeat));
        practice = std::make_unique<THGuiPrac>();
        overlay = std::make_unique<THOverlay>();
        advanced = std::make_unique<THAdvanced>();
        pause = std::make_unique<THPauseMenu>();
    }
    ImGui::SetCurrentContext(guiContext);
    auto language=static_cast<Locale>(PracticeLanguage());
    if(Gui::LocaleGet()!=language)Gui::LocaleSet(language);
    if (wanted && !opened) {
        practice->State(1);
        opened = true;
        openingFrames = 0;
    }
    if (!wanted && opened) {
        practice->State(action == 1 ? 3 : 4);
        opened = false;
    }
    bool anyVisible=wanted||quickWanted||advancedWanted||pauseWanted;
    if(anyVisible&&!inputHooked){
        // Releases can arrive while the overlay's WndProc is detached (e.g.
        // Enter after starting practice). Do not carry held inputs into the
        // next menu, where a stale Enter would immediately close numeric input.
        auto& io = ImGui::GetIO();
        for(auto& key : io.KeysDown)key=false;
        for(auto& button : io.MouseDown)button=false;
        io.ClearInputCharacters();
        ImGui::ClearActiveID();
        inputHooked=Gui::ImplWin32HookWndProc();
    }
    if(!anyVisible) {
        ImGui::GetIO().MouseDrawCursor=false;
        if(inputHooked){Gui::ImplWin32UnHookWndProc();inputHooked=false;}
    }
    if(!anyVisible && !displayNotice) {
        ImGui::SetCurrentContext(previousContext);
        return PresentFrame(swap, interval, flags);
    }
    auto native = *reinterpret_cast<uint16_t*>(imageBase + Rva::Input);
    auto prev = *reinterpret_cast<uint16_t*>(imageBase + Rva::PreviousInput);
    uiInput = (native & 0xf4) | ((native & 0x100) ? 1 : 0);
    uiPrevious = (prev & 0xf4) | ((prev & 0x100) ? 1 : 0);
    uiRepeat = *reinterpret_cast<uint16_t*>(imageBase + Rva::InputRepeat);
    auto& io = ImGui::GetIO();
    // NC hides the OS cursor. Draw it in the same scaled coordinate space as
    // the widgets, without changing the game's ShowCursor counter.
    io.MouseDrawCursor=wanted||pauseWanted||advancedWanted;
    RECT client{};
    GetClientRect(desc.OutputWindow, &client);
    float screenScale = std::max(1.0f, float(client.bottom)) / 480.0f;
    io.DisplaySize = {float(client.right) / screenScale, 480};
    ImGui_ImplDX11_NewFrame();
    Gui::ImplWin32NewFrame();
    Gui::KeyboardInputUpdate();
    // ImGui consumes navigation inputs in NewFrame, before widgets are drawn.
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.NavInputs[ImGuiNavInput_DpadUp] = Gui::InGameInputGet(VK_UP);
    io.NavInputs[ImGuiNavInput_DpadDown] = Gui::InGameInputGet(VK_DOWN);
    io.NavInputs[ImGuiNavInput_DpadLeft] = Gui::InGameInputGet(VK_LEFT);
    io.NavInputs[ImGuiNavInput_DpadRight] = Gui::InGameInputGet(VK_RIGHT);
    ImGui::NewFrame();
    if(wanted)practice->Update();
    if(pauseWanted){
        pause->SetPos((io.DisplaySize.x-384.0f)*0.5f,16.0f);
        pauseAction=pause->PMState();
        pause->Update();
        if(pauseAction)pauseWanted=false;
    }
    if(quickWanted){overlay->Open();overlay->Update();}else overlay->Close();
    if(advancedWanted){advanced->Open();advanced->Update();}else advanced->Close();
    if(displayNotice) {
        ImGui::SetNextWindowPos({12,468},ImGuiCond_Always,{0,1});
        ImGui::SetNextWindowSize({std::min(480.0f,io.DisplaySize.x-24.0f),0},ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.9f);
        if(ImGui::Begin("##nc_display_notice",nullptr,ImGuiWindowFlags_NoDecoration|
            ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings))
            ImGui::TextWrapped("%s",S(THPRAC_GAMES_LOW_LATENCY_UNAVAILABLE));
        ImGui::End();
    }
    if (wanted && ++openingFrames > 12 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        if ((native & 0x100) && !(prev & 0x100) && (!ImGui::IsAnyItemActive() || NumericInputActive())) {
            // Numeric widgets have already applied this frame's edits. Z also
            // confirms them and starts practice without an extra focus change.
            ImGui::ClearActiveID();
            practice->State(3);
            action = 1;
            wanted = false;
        } else if ((native & 0x200) && !(prev & 0x200) && !ImGui::IsAnyItemActive()) {
            action = 2;
            wanted = false;
        }
    }
    ImGui::Render();
    ID3D11Texture2D* buffer{};
    ID3D11RenderTargetView* target{};
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) {
        D3D11_TEXTURE2D_DESC surface{};
        buffer->GetDesc(&surface);
        device->CreateRenderTargetView(buffer, nullptr, &target);
        buffer->Release();
        if (target) {
            auto data = ImGui::GetDrawData();
            ImVec2 scale{surface.Width / io.DisplaySize.x, surface.Height / io.DisplaySize.y};
            data->ScaleClipRects(scale);
            for (int n = 0; n < data->CmdListsCount; ++n)
                for (auto& vertex : data->CmdLists[n]->VtxBuffer) {
                    vertex.pos.x *= scale.x;
                    vertex.pos.y *= scale.y;
                }
            data->DisplaySize = {float(surface.Width), float(surface.Height)};
            ID3D11RenderTargetView* saved[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView* depth{};
            context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved, &depth);
            context->OMSetRenderTargets(1, &target, nullptr);
            ImGui_ImplDX11_RenderDrawData(data);
            context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved, depth);
            for (auto view : saved) if (view) view->Release();
            if (depth) depth->Release();
            target->Release();
        }
    }
    ImGui::SetCurrentContext(previousContext);
    return PresentFrame(swap, interval, flags);
}
}
THGuiPrac& PracticeUi(){return *practice;}
void OpenPause(){pauseWanted=true;pauseAction=0;}
void ClosePause(){pauseWanted=false;}
int TakePauseAction(){int result=pauseAction;pauseAction=0;return result;}
void OpenPractice() { wanted = true; action = 0; }
void ClosePractice() { wanted = false; }
bool PracticeIsOpen() { return wanted || opened; }
int TakePracticeAction() { int result = action; action = 0; return result; }
void ToggleQuickMenu(){if(PracticeActive()&&!NumericInputActive())quickWanted=!quickWanted;}
void ToggleAdvancedMenu(){if(PracticeActive())advancedWanted=!advancedWanted;}
using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
CreateDeviceFn originalCreateDevice{};
bool overlayLowLatency{};
bool overlayInstalled{};
void TryInstallPresentHook(IDXGISwapChain* swap) {
    if (overlayInstalled || !swap) return;
    auto entry = (*reinterpret_cast<void***>(swap))[8];
    MH_STATUS st=MH_CreateHook(entry, reinterpret_cast<void*>(Present), reinterpret_cast<void**>(&originalPresent));
    if (st == MH_OK) {
        if (overlayLowLatency) InstallLowLatency((*reinterpret_cast<void***>(swap))[13]);
        MH_EnableHook(entry);
        overlayInstalled = true;
        DiagLog("overlay: present hooked");
    } else { char buf[64]; snprintf(buf,sizeof(buf),"overlay: present hook fail %d",st); DiagLog(buf); }
}
HRESULT WINAPI HookedCreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE b, HMODULE c, UINT d,
    const D3D_FEATURE_LEVEL* e, UINT f, UINT g, const DXGI_SWAP_CHAIN_DESC* h, IDXGISwapChain** pSwap,
    ID3D11Device** i, D3D_FEATURE_LEVEL* j, ID3D11DeviceContext** k) {
    HRESULT hr = originalCreateDevice(a,b,c,d,e,f,g,h,pSwap,i,j,k);
    if (SUCCEEDED(hr) && pSwap && *pSwap) TryInstallPresentHook(*pSwap);
    return hr;
}
// The game may have created its device before the creation hook went in (injection
// races with the game's own startup), or may use D3D11CreateDevice plus
// IDXGIFactory::CreateSwapChain. Present is one function shared by every swap chain
// of the DXGI implementation, so hooking it through a throwaway swap chain also
// covers the game's own.
bool InstallPresentHookFromDummy() {
    DiagLog("overlay: fallback dummy swap chain");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"ThpracNcDummyWindow";
    RegisterClassExW(&wc);
    HWND hwnd=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
    if(!hwnd){DiagLog("overlay: dummy window fail");return false;}
    DiagLog("overlay: dummy window ok");
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount=1; sd.BufferDesc.Width=64; sd.BufferDesc.Height=64;
    sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow=hwnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap{}; ID3D11Device* dev{}; ID3D11DeviceContext* ctx{}; D3D_FEATURE_LEVEL level{};
    // Call the trampoline so HookedCreateDevice is not re-entered.
    HRESULT hr=originalCreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&swap,&dev,&level,&ctx);
    char buf[64]; snprintf(buf,sizeof(buf),"overlay: dummy create device hr=0x%08lx",(unsigned long)hr); DiagLog(buf);
    bool ok=false;
    if(SUCCEEDED(hr)&&swap){TryInstallPresentHook(swap);ok=overlayInstalled;}
    if(swap)swap->Release();
    if(ctx)ctx->Release();
    if(dev)dev->Release();
    DestroyWindow(hwnd);
    return ok;
}
bool InstallOverlay(bool lowLatency)
{
    DiagLog("overlay: begin");
    overlayLowLatency = lowLatency;
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    for(int i=0;i<600 && !d3d11;i++){Sleep(50); d3d11=GetModuleHandleW(L"d3d11.dll");}
    if (!d3d11) { DiagLog("overlay: d3d11 not loaded"); return false; }
    DiagLog("overlay: d3d11 loaded");
    auto proc = GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    if (!proc) { DiagLog("overlay: no proc"); return false; }
    MH_STATUS st=MH_CreateHook(reinterpret_cast<void*>(proc), reinterpret_cast<void*>(HookedCreateDevice), reinterpret_cast<void**>(&originalCreateDevice));
    if (st != MH_OK) { char buf[64]; snprintf(buf,sizeof(buf),"overlay: createhook fail %d",st); DiagLog(buf); return false; }
    st=MH_EnableHook(reinterpret_cast<void*>(proc));
    if (st != MH_OK) { char buf[64]; snprintf(buf,sizeof(buf),"overlay: enable fail %d",st); DiagLog(buf); return false; }
    DiagLog("overlay: hooked create");
    // Give the game time to call the hooked function itself; if it did not, hook Present directly.
    for(int i=0;i<60 && !overlayInstalled;++i)Sleep(50);
    if(!overlayInstalled)InstallPresentHookFromDummy();
    return true;
}
}
// ---- Practice runtime (th06nc/runtime.cpp) ----

namespace {
bool addedSpellBgm(const Settings& settings);
uintptr_t base{}; Shared* shared{}; HANDLE mutex{}, mapping{};
LaunchOptions launchOptions{};
using CalcCallback = int64_t(*)(uint32_t*);
CalcCallback originalCalc{};
Settings config{}, requested{}, initialConfig{}; Status status{};
std::atomic<int> language{1};
bool active=false, playback=false, armed=false;
uint32_t resourceLocks=0;
int lockedLives=0,lockedBombs=0,lockedPower=0;
bool retryPending=false, speedOwned=false;
double originalFps=60.0;
bool initializingPractice{}, practicePaused{};
int pausedBgm=-1;
GameBuild gGameBuild=GameBuild::Unsupported;
int selectedDifficulty{};
using PlayBgm= int64_t(*)(void*,const char*); PlayBgm originalBgm{};
void* practiceMenu{};int practiceTransitionMode{};bool confirmNativePractice{};
using Transition=void(*)(void*,int,int); Transition originalTransition{};using MenuCallback=int64_t(*)(void*); MenuCallback originalMenu{};
uint32_t restartSeen=0;
uint32_t simulationTick=0;
uint32_t playbackEndTick=0;
struct Event { uint32_t tick, flags; int enabled; };
std::vector<Event> events; size_t nextEvent=0;
bool diagnostics=false;HANDLE trace=INVALID_HANDLE_VALUE;uint32_t tracedTick=UINT32_MAX;
struct ReplayHeader { uint32_t magic=0x4e435250, version=ReplayProtocol; Settings settings; uint64_t digest=0; uint32_t count=0,endTick=0; };
using Callback=int64_t(*)(void*);
Callback originalInit{}, originalUpdate{}, originalPlayer{}, originalEnemy{}, originalSupervisor{}, originalResultInit{};
using EclCallback=int64_t(*)(void*,void*);
EclCallback originalEcl{};
using VoidEcl=void(*)(void*,void*); VoidEcl originalFinal{};
using FileWriter=void(*)(const char*,void*,size_t);FileWriter originalWrite{};
template<class T> T& mem(uintptr_t rva) { return *reinterpret_cast<T*>(base+rva); }
template<class T> T& field(void* p,size_t offset) { return *reinterpret_cast<T*>(static_cast<char*>(p)+offset); }
void pausePracticeBgm() {
    pausedBgm=-1;
    int handle=mem<int>(Rva::BgmHandle);
    // Match native GameUpdate: StopSound(handle, 0) preserves the cursor.
    if(!mem<uint8_t>(Rva::SpellPractice) && handle!=-1 &&
       reinterpret_cast<int(*)(int,int)>(base+Rva::StopSound)(handle,0)==0)pausedBgm=handle;
}
void resumePracticeBgm() {
    // Native pause resume uses the current loop mode and does not rewind.
    // Never restart a handle replaced by a native scene/music transition.
    if(pausedBgm!=-1 && pausedBgm==mem<int>(Rva::BgmHandle))
        reinterpret_cast<int(*)(int,int,int)>(base+Rva::PlaySound)(pausedBgm,mem<uint8_t>(Rva::BgmLoop)?3:1,0);
    pausedBgm=-1;
}
void sync() {
    if(!shared) return;
    DWORD lock=WaitForSingleObject(mutex,0);
    if(lock!=WAIT_OBJECT_0 && lock!=WAIT_ABANDONED)return;
    if(shared->magic==Magic&&shared->version==Protocol) {
        if(valid(shared->settings))requested=shared->settings;
        if(shared->language>=0&&shared->language<=2)language.store(shared->language,std::memory_order_relaxed);
    }

    shared->status=status;
    shared->practiceEnabled=launchOptions.practice;
    shared->lowLatencyState=static_cast<int>(LowLatencyState());
    shared->lowLatencyError=LowLatencyError();
    ReleaseMutex(mutex);
}
int64_t calc(uint32_t* result) {
    LowLatencyBeforeFrame();
    if(!launchOptions.practice)sync(); // Practice's supervisor already synchronizes this frame.
    return originalCalc(result);
}
void message(const wchar_t* text) { wcsncpy_s(status.message,text,_TRUNCATE); }
bool bossBgm(const Settings& settings) {
    if(settings.stage==7 && IsAddedSpell(settings.section))return true;
    return settings.section>0 && settings.section<int(std::size(THPrac::TH06::th_sections_bgm)) &&
        THPrac::TH06::th_sections_bgm[settings.section];
}
uint64_t digest(const void* data,size_t size) {
    uint64_t value=14695981039346656037ULL;
    for(size_t i=0;i<size;++i)value=(value^static_cast<const uint8_t*>(data)[i])*1099511628211ULL;
    return value;
}
bool readFile(const std::string& name,std::vector<uint8_t>& out,size_t limit) {
    HANDLE f=CreateFileA(name.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};bool ok=GetFileSizeEx(f,&size)&&size.QuadPart>=0&&size.QuadPart<=int64_t(limit);
    DWORD got=0;if(ok){out.resize(size_t(size.QuadPart));ok=ReadFile(f,out.data(),DWORD(out.size()),&got,nullptr)&&got==out.size();}
    CloseHandle(f);return ok;
}
bool loadReplay() {
    const char* name=reinterpret_cast<const char*>(base+Rva::ReplayPath);
    if(strnlen_s(name,260)==260)return false;
    std::vector<uint8_t> sidecar,raw;
    if(!readFile(std::string(name)+".thprac-nc",sidecar,16*1024*1024)||sidecar.size()<sizeof(ReplayHeader))return false;
    ReplayHeader header{};memcpy(&header,sidecar.data(),sizeof(header));
    if(header.magic!=0x4e435250||header.version!=ReplayProtocol||!valid(header.settings)||
       header.count>(16*1024*1024)/sizeof(Event)||sidecar.size()!=sizeof(header)+size_t(header.count)*sizeof(Event))return false;
    if(!readFile(name,raw,64*1024*1024)||digest(raw.data(),raw.size())!=header.digest)return false;
    events.resize(header.count);memcpy(events.data(),sidecar.data()+sizeof(header),events.size()*sizeof(Event));
    uint32_t tick=0;for(auto& e:events){if(e.tick<tick||(e.flags&~255u)||(e.enabled!=0&&e.enabled!=1))return false;tick=e.tick;}
    config=header.settings;nextEvent=0;playbackEndTick=header.endTick;
    return true;
}
void writeFile(const char* name,void* data,size_t size) {
    auto n=strlen(name);
    if(n>=4&&_stricmp(name+n-4,".rpy")==0&&active&&!playback) {
        ReplayHeader header;header.settings=initialConfig;header.digest=digest(data,size);header.count=uint32_t(events.size());header.endTick=simulationTick;
        std::string target=std::string(name)+".thprac-nc",tmp=target+".tmp";
        HANDLE f=CreateFileA(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);DWORD wrote=0;
        bool ok=f!=INVALID_HANDLE_VALUE;
        if(ok){ok=WriteFile(f,&header,sizeof(header),&wrote,nullptr)&&wrote==sizeof(header);
          if(ok&&!events.empty())ok=WriteFile(f,events.data(),DWORD(events.size()*sizeof(Event)),&wrote,nullptr)&&wrote==events.size()*sizeof(Event);
          FlushFileBuffers(f);CloseHandle(f);}
        if(ok)ok=MoveFileExA(tmp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
        if(!ok)message(L"录像参数文件保存失败");else {++status.exports;message(L"练习录像及参数已保存");}
    }
    originalWrite(name,data,size);
}
// TH06 routes practice exits to the native result screen's save question.
// NC normally skips that screen in Practice mode. Initialize its assets in
// state 9 (without adding a high score), then enter the native save flow.
int64_t resultInit(void* p) {
    bool practiceResult=active && !playback && mem<int>(Rva::NextState)==7;
    if(practiceResult)field<int>(p,0x9e94)=9;
    auto result=originalResultInit(p);
    if(practiceResult && result==0) {
        field<int>(p,0x9e94)=10;
        field<int>(p,0x4450)=0;
        memset(static_cast<char*>(p)+0x9e88,' ',8);
        field<char>(p,0x9e90)=0;
    }
    return result;
}
void hotkeys() {
    DWORD owner=0;GetWindowThreadProcessId(GetForegroundWindow(),&owner);
    static bool last[256]{};bool pressed[256]{};
    for(int key=0;key<256;++key){bool down=(GetAsyncKeyState(key)&0x8000)!=0;pressed[key]=down&&!last[key]&&owner==GetCurrentProcessId();last[key]=down;}
    uint32_t toggle=0;for(int i=0;i<6;++i)if(pressed[VK_F1+i])toggle|=1u<<i;
    if(pressed[VK_F7])toggle|=KeepBgm;
    bool restart=pressed['R']&&(GetAsyncKeyState(VK_ESCAPE)&0x8000);
    if(pressed[VK_BACK])ToggleQuickMenu();
    if(pressed[VK_F11])ToggleAdvancedMenu();
    if((toggle||restart)&&!playback){auto lock=WaitForSingleObject(mutex,0);if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED){
      shared->settings.flags^=toggle;if(restart)++shared->settings.restart;requested=shared->settings;ReleaseMutex(mutex);}}

}
void speed(double fps) {
    if(mem<double>(Rva::Fps)==double(fps))return;
    auto frequency=mem<int64_t>(Rva::CounterFrequency);if(frequency<=0)return;
    auto period=std::max<int64_t>(1,int64_t(double(frequency)/fps+0.5));
    mem<double>(Rva::Fps)=double(fps);mem<int64_t>(Rva::FramePeriod)=period;
    mem<int64_t>(Rva::HalfWait)=std::min<int64_t>(period/2,int64_t(frequency*mem<double>(Rva::WaitFactor)));
    mem<uint8_t>(Rva::ResetFrameTimer)=0;
}
int64_t supervisor(void* p){
    sync();
    // Retry temporarily sets nextState to zero before registering GameInit.
    // Remember ownership before the native teardown consumes that transition.
    if(active && field<int>(p,0x428)==2 && field<int>(p,0x42c)==12)retryPending=true;
    bool inPractice=active && field<int>(p,0x428)==2 && field<int>(p,0x42c)==2;
    if(inPractice){
        hotkeys();
        if(!speedOwned){originalFps=mem<double>(Rva::Fps);speedOwned=true;}
        speed(requested.enabled?requested.fps:originalFps);
    } else if(speedOwned){speed(originalFps);speedOwned=false;}
    return originalSupervisor(p);
}
void resources() {
    mem<int8_t>(Rva::Lives)=int8_t(config.lives);mem<int8_t>(Rva::Bombs)=int8_t(config.bombs);
    mem<int16_t>(Rva::Power)=int16_t(config.power);
    mem<uint64_t>(Rva::DisplayScore)=mem<uint64_t>(Rva::Score)=config.score;
    mem<int32_t>(Rva::Graze)=mem<int32_t>(Rva::StageGraze)=config.graze;
    mem<uint16_t>(Rva::Point)=mem<uint16_t>(Rva::StagePoint)=uint16_t(config.point);
    if(config.flags&CustomRank)mem<int>(Rva::Rank)=config.rank;
    int extensions=0; while(extensions<5 && config.score>=mem<uint32_t>(Rva::ExtendScores+4*extensions))++extensions;
    mem<uint8_t>(Rva::Extends)=uint8_t(extensions);
}
void restoreResourceLocks() {
    if(resourceLocks&InfiniteLives)mem<int8_t>(Rva::Lives)=int8_t(lockedLives);
    if(resourceLocks&InfiniteBombs)mem<int8_t>(Rva::Bombs)=int8_t(lockedBombs);
    if(resourceLocks&InfinitePower)mem<int16_t>(Rva::Power)=int16_t(lockedPower);
}
void updateResourceLocks() {
    uint32_t next=active&&config.enabled?config.flags&(InfiniteLives|InfiniteBombs|InfinitePower):0;
    uint32_t rising=next&~resourceLocks;
    if(rising&InfiniteLives)lockedLives=mem<int8_t>(Rva::Lives);
    if(rising&InfiniteBombs)lockedBombs=mem<int8_t>(Rva::Bombs);
    if(rising&InfinitePower)lockedPower=mem<int16_t>(Rva::Power);
    resourceLocks=next;restoreResourceLocks();
}
void skipStageIntroduction() {
    if(config.section==0 && config.frame==0)return;
    auto gui=mem<void*>(Rva::Gui);
    if(gui){
        // The native Spell Practice initializer hides this same stage-name VM.
        auto title=static_cast<char*>(gui)+0x960;
        field<uint32_t>(title,0xc4)&=~1u;
        field<uintptr_t>(title,0xf0)=field<uintptr_t>(title,0xf8)=0;
    }
    auto playerObject=reinterpret_cast<void*>(base+Rva::Player);
    // At GameInit the player is still in state 1 (birth). Its first update
    // would enter state 3 with 240 invulnerability frames. Skip that transition.
    field<uint8_t>(playerObject,0x7898)=0;
    field<int>(playerObject,0x7854)=-999;
    field<int>(playerObject,0x7858)=0;
    field<uint32_t>(playerObject,0x798c)&=~12u;
    field<uint32_t>(playerObject,0x79b4)=0xffffffff;
    field<float>(playerObject,0x79ac)=field<float>(playerObject,0x79b0)=1.0f;
}
int64_t init(void* p) {
    sync();playback=mem<uint8_t>(Rva::Replay)!=0;resourceLocks=0;
    bool vanillaRetry=!playback && retryPending && !requested.enabled;
    if(playback)active=loadReplay();
    else {
        bool owned=retryPending || (armed && mem<uint8_t>(Rva::Practice) && !mem<uint8_t>(Rva::SpellPractice));
        if(owned)config=requested;
        active=owned && requested.enabled;
        events.clear();nextEvent=0;
    }
    armed=false;retryPending=false;practicePaused=false;pausedBgm=-1;ClosePause();
    initialConfig=config;simulationTick=0;restartSeen=requested.restart;
    if(active || vanillaRetry) {
        mem<int>(Rva::Stage)=config.stage-1;
        mem<int>(Rva::Difficulty)=config.stage==7?4:std::min(config.difficulty,3);
        mem<uint8_t>(Rva::Character)=uint8_t(config.shot/2);mem<uint8_t>(Rva::Shot)=uint8_t(config.shot%2);
        mem<uint8_t>(Rva::Practice)=1;
        mem<uint8_t>(Rva::SpellPractice)=0;

    }
    initializingPractice=active;
    auto result=originalInit(p);
    initializingPractice=false;
    if(active && result==0) {
        if(!(config.flags&CustomRank))config.rank=mem<int>(Rva::Rank);
        initialConfig=config;
        resources();
        thPracParam.stage=config.stage-1;thPracParam.section=config.section;thPracParam.frame=config.frame;
        thPracParam.dlg=config.dialogue;thPracParam.fakeType=config.fakeShot+1;thPracParam.phase=config.phase;
        if(!ApplyEcl()){active=false;status.error=7;message(L"脚本映射校验失败，未应用练习选段。");}
        if(!active){mem<int>(Rva::NextState)=8;sync();return result;}
        status.error=0;
        skipStageIntroduction();
        updateResourceLocks();
        if(active && EclNeedsBossAssets()) {
            using LoadAnm=int64_t(*)(void*,int,const char*,int);
            auto load=reinterpret_cast<LoadAnm>(base+Rva::LoadAnm);
            if(config.stage==6)load(nullptr,11,"data/eff06.anm",0x2d3);
            if(config.stage==7){load(nullptr,11,"data/eff07.anm",0x2d3);load(nullptr,18,"data/face12c.anm",0x4c6);}
        }
        ++status.starts;message(playback?L"已恢复练习录像参数":L"练习参数已应用");restartSeen=requested.restart;
        if(trace!=INVALID_HANDLE_VALUE){CloseHandle(trace);trace=INVALID_HANDLE_VALUE;}
        tracedTick=UINT32_MAX;
        if(diagnostics){auto name=L"practice-trace-"+std::to_wstring(status.starts)+(playback?L"-replay.csv":L"-record.csv");
          trace=CreateFileW(name.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);}
    } else active=false;
    sync();return result;
}
int64_t ecl(void* manager,void* enemyObject) {
    if(!(active&&config.enabled&&config.fakeShot>=0))return originalEcl(manager,enemyObject);
    auto character=mem<uint8_t>(Rva::Character),shot=mem<uint8_t>(Rva::Shot);
    mem<uint8_t>(Rva::Character)=uint8_t(config.fakeShot/2);mem<uint8_t>(Rva::Shot)=uint8_t(config.fakeShot%2);
    auto result=originalEcl(manager,enemyObject);
    mem<uint8_t>(Rva::Character)=character;mem<uint8_t>(Rva::Shot)=shot;return result;
}
void selectParameters() {
    Settings selected{};selected.enabled=thPracParam.mode;selected.stage=thPracParam.stage+1;
    selected.difficulty=selectedDifficulty;selected.shot=MenuShot();
    selected.section=thPracParam.section;selected.frame=thPracParam.frame;
    selected.lives=int(thPracParam.life);selected.bombs=int(thPracParam.bomb);selected.power=int(thPracParam.power);
    selected.score=thPracParam.score;selected.graze=thPracParam.graze;selected.point=thPracParam.point;
    selected.rank=thPracParam.rank;selected.flags=(requested.flags&~CustomRank)|(thPracParam.customRank?CustomRank:0);
    selected.fakeShot=thPracParam.fakeType-1;selected.dialogue=thPracParam.dlg;selected.phase=thPracParam.phase;
    selected.restart=requested.restart;selected.fps=requested.fps;
    requested=selected;
    auto lock=WaitForSingleObject(mutex,100);if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED){shared->settings=requested;ReleaseMutex(mutex);}
}
int64_t update(void* p) {
    sync();
    if(active && playback && playbackEndTick && simulationTick>=playbackEndTick){mem<int>(Rva::NextState)=8;return 3;}
    if(active) {
        uint32_t tick=simulationTick;
        if(playback){while(nextEvent<events.size()&&events[nextEvent].tick<=tick){auto e=events[nextEvent++];config.flags=e.flags;config.enabled=e.enabled;}}
        else {
            // Rank belongs to the starting parameters; pause edits apply on retry.
            auto flags=(requested.flags&~CustomRank)|(config.flags&CustomRank);
            if(config.flags!=flags||config.enabled!=requested.enabled){
                config.flags=flags;config.enabled=requested.enabled;
                events.push_back({tick,config.flags,config.enabled});
            }
        }
    }
    if(active&&!playback) {
        if(requested.restart!=restartSeen) {
            resumePracticeBgm();
            restartSeen=requested.restart;
            retryPending=true;
            mem<uint8_t>(Rva::KeepBgm)=uint8_t((config.flags&KeepBgm)&&requested.stage==config.stage&&bossBgm(config)==bossBgm(requested)&&addedSpellBgm(config)==addedSpellBgm(requested));
            // The native state machine handles teardown/recreation, including all callbacks.
            mem<int>(Rva::NextState)=12; // Native retry path; 3 is next-stage and reuses unloaded ANMs.
            return 3;
        }
    }
    updateResourceLocks();
    if(active && !playback && mem<int>(Rva::NextState)==2) {
        if(!practicePaused && !mem<uint8_t>(Rva::Paused) && !mem<uint8_t>(Rva::GameOver) &&
           ((mem<uint16_t>(Rva::Input)&0x400) && !(mem<uint16_t>(Rva::PreviousInput)&0x400))) {
            practicePaused=true;pausePracticeBgm();OpenPause();
        }
        if(practicePaused) {
            int action=TakePauseAction();
            if(action){
                practicePaused=false;ClosePause();
                mem<uint16_t>(Rva::PreviousInput)|=0x400;
                if(action==2){pausedBgm=-1;mem<int>(Rva::NextState)=7;return 3;}
                resumePracticeBgm();
                if(action==3){selectParameters();RequestPracticeRestart();return 3;}
            } else {
                // Keep native render preparation, and stop the update chain just
                // as the game's pause path does. Its menu state stays untouched.
                auto paused=mem<uint8_t>(Rva::Paused);
                mem<uint8_t>(Rva::Paused)=1;
                auto result=originalUpdate(p);
                mem<uint8_t>(Rva::Paused)=paused;
                return result;
            }
        }
    }
    auto result=originalUpdate(p);
    status.stage=mem<int>(Rva::Stage);status.difficulty=mem<int>(Rva::Difficulty);status.frame=mem<int>(Rva::TimelineFrame);
    status.lives=mem<int8_t>(Rva::Lives);status.bombs=mem<int8_t>(Rva::Bombs);status.power=mem<int16_t>(Rva::Power);
    status.rank=mem<int>(Rva::Rank);status.score=mem<uint64_t>(Rva::Score);
    status.flags=active?config.flags:0;
    status.misses=mem<int>(Rva::Misses);status.bombsUsed=mem<int>(Rva::BombsUsed);
    return result;
}
int64_t player(void* p) {
    bool enabled=active&&config.enabled;
    if(enabled&&(config.flags&Invincible) && (field<uint8_t>(p,0x7898)==0||field<uint8_t>(p,0x7898)==3)) {
        field<uint8_t>(p,0x7898)=3;field<int>(p,0x7858)=2;
    }
    if(enabled&&(config.flags&AutoBomb)&&field<uint8_t>(p,0x7898)==2&&mem<int8_t>(Rva::Bombs)>0) {
        mem<uint16_t>(Rva::Input)|=2;mem<uint16_t>(Rva::PreviousInput)&=~2;
    }
    // Let a locked last life take the normal miss/respawn path, without letting
    // the native zero-life branch end the practice. Restore before HUD draw.
    if((resourceLocks&InfiniteLives)&&lockedLives==0)mem<int8_t>(Rva::Lives)=1;
    auto result=originalPlayer(p);
    restoreResourceLocks();
    return result;
}
int64_t enemy(void* p) {
    // Timer locking is applied around the actual enemy callback, on the game thread.
    int timers[256]{};int timelineBefore=mem<int>(Rva::TimelineFrame);bool locked=active&&config.enabled&&(config.flags&TimeLock);
    if(locked)for(int i=0;i<256;i++)timers[i]=field<int>(p,8+i*0x10b0+4);
    auto result=originalEnemy(p);
    if(locked)for(int i=0;i<256;i++) {
        auto off=8+i*0x10b0;
        if(field<uint8_t>(p,off+0xbc)&0x80)field<int>(p,off+4)=timers[i];
    }
    if(locked && config.stage<=5 && config.stage!=3) {
        // Match TH06's midboss-only workaround. Boss introductions must keep
        // advancing so timeline interrupts can enter the actual attack.
        constexpr int starts[]={2008,2588,0,4132,3374};
        constexpr int lengths[]={48*60,32*60,0,40*60,70*60};
        constexpr int waits[]={4*60,15*60,0,12*60,5*60};
        int st=config.stage-1,start=NativeTimelineFrame(config.stage,starts[st]);
        if(timelineBefore>=start && timelineBefore<start+lengths[st])
            for(int i=0;i<256;++i)if((field<uint8_t>(p,8+i*0x10b0+0xbc)&0x80)&&(field<uint8_t>(p,8+i*0x10b0+0xbd)&8)){
                mem<int>(Rva::TimelineFrame)=std::max(timelineBefore,start+waits[st]);break;
            }
    }
    if(trace!=INVALID_HANDLE_VALUE){auto tick=simulationTick;
      if(tick%60==0&&tick!=tracedTick){tracedTick=tick;uint64_t hash=0;
        for(int i=0;i<256;++i){auto off=8+i*0x10b0;if(field<uint8_t>(p,off+0xbc)&0x80){
          hash=hash*1099511628211ULL^digest(static_cast<char*>(p)+off+0xb0,12);
          hash=hash*1099511628211ULL^uint64_t(field<int>(p,off+0x234));
          hash=hash*1099511628211ULL^uint64_t(field<int>(p,off+4));
          hash=hash*1099511628211ULL^uint64_t(field<int16_t>(p,off+0x80));}}
        char line[256];auto length=snprintf(line, sizeof(line),"%u,%llu,%u,%u,%d,%d,%d,%llu\n",tick,hash,mem<uint16_t>(Rva::RandomSeed),mem<uint32_t>(Rva::RandomCount),mem<int8_t>(Rva::Lives),mem<int8_t>(Rva::Bombs),mem<int32_t>(Rva::StageGraze),mem<uint64_t>(Rva::Score));
        DWORD written;WriteFile(trace,line,DWORD(length),&written,nullptr);
      }}
    ++simulationTick;return result;
}
void finalPhase(void* enemyObject,void* instr) {
    auto age=field<int>(enemyObject,4);
    if(active&&config.enabled&&config.stage==7&&config.section==THPrac::TH06::TH06_ST7_END_S10&&config.phase==1)field<int>(enemyObject,4)=7200;
    originalFinal(enemyObject,instr);field<int>(enemyObject,4)=age;
}
void transition(void* menuObject,int origin,int mode) {
    if(origin==0x16 && field<uint8_t>(menuObject,0x168d1) && !field<uint8_t>(menuObject,0x168d3)) {
        selectedDifficulty=std::clamp(MenuDifficulty(),0,3);
        practiceMenu=menuObject;practiceTransitionMode=mode;OpenPractice();return;
    }
    originalTransition(menuObject,origin,mode);
}
// Spells from "Fragile Wing" onward (the added spells) use th06_16 instead of
// the stage's boss theme.
bool addedSpellBgm(const Settings& settings) { return settings.stage==7 && IsAddedSpell(settings.section); }
const char* addedSpellBgmPath(const char* path) {
    static char buffer[260];
    if(!path||strnlen_s(path,sizeof(buffer))>=sizeof(buffer))return path;
    strcpy(buffer,path);
    char* number=strstr(buffer,"th06_");
    if(!number||number[5]<'0'||number[5]>'9'||number[6]<'0'||number[6]>'9')return path;
    number[5]='1';number[6]='6';
    return buffer;
}
// The added spells play th06_16, whose ~12 s orchestral lead-in drags in spell
// practice. Right after the game starts the fresh stream, rewind its cursor
// fields to 11.9 s and let it resume there, mirroring the pause/resume path.
// Streaming-sound cursor fields hold decoded PCM bytes: 48 kHz / 16-bit /
// stereo => 192000 B/s; 11.9 s lands exactly on a 20 ms opus frame boundary.
constexpr double kAddedSpellBgmStart=11.9;
constexpr int64_t kBgmBytesPerSec=192000;
void restartAddedSpellBgm() {
    int handle=mem<int>(Rva::BgmHandle);
    if(handle==-1||!Rva::SoundTable)return;
    void** table=*reinterpret_cast<void***>(base+Rva::SoundTable);
    if(!table)return;
    auto sound=static_cast<uint8_t*>(table[handle & 0x3ffff]);
    if(!sound)return;
    int64_t offset=static_cast<int64_t>(kAddedSpellBgmStart*kBgmBytesPerSec);
    *reinterpret_cast<int64_t*>(sound+0x2c38)=offset;
    *reinterpret_cast<int64_t*>(sound+0x2c50)=offset;
    *reinterpret_cast<uint8_t*>(sound+0x2c5c)=0;
    *reinterpret_cast<uint16_t*>(sound+0x2c5e)=0;
    *reinterpret_cast<int*>(sound+0x2c68)=0;
    reinterpret_cast<int(*)(int,int)>(base+Rva::StopSound)(handle,0);
    reinterpret_cast<int(*)(int,int,int)>(base+Rva::PlaySound)(handle,mem<uint8_t>(Rva::BgmLoop)?3:1,0);
}int64_t playBgm(void* supervisor,const char* path) {
    bool addedSpell=initializingPractice && bossBgm(config) && addedSpellBgm(config);
    if(initializingPractice && bossBgm(config)) {
        auto stageData=mem<uintptr_t>(Rva::StageData);
        if(stageData)path=reinterpret_cast<const char*>(stageData+0x310);
        if(addedSpell)path=addedSpellBgmPath(path);
    }
    int64_t ret=originalBgm(supervisor,path);
    if(addedSpell && gGameBuild==GameBuild::Steam25306795)restartAddedSpellBgm();
    return ret;
}
int64_t menu(void* p) {
    if(practiceMenu==p) {
        int action=TakePracticeAction();
        if(!action) {
            // The native callback returns 1 to remain in the chain. Keep its
            // animation/render work running, while masking stage-menu input.
            auto input=mem<uint16_t>(Rva::Input),previous=mem<uint16_t>(Rva::PreviousInput),repeat=mem<uint16_t>(Rva::InputRepeat);
            mem<uint16_t>(Rva::Input)=mem<uint16_t>(Rva::PreviousInput)=mem<uint16_t>(Rva::InputRepeat)=0;
            auto result=originalMenu(p);
            mem<uint16_t>(Rva::Input)=input;mem<uint16_t>(Rva::PreviousInput)=previous;mem<uint16_t>(Rva::InputRepeat)=repeat;
            return result;
        }
        practiceMenu=nullptr;
        if(action==1) {
            selectParameters();
            armed=requested.enabled!=0;
            confirmNativePractice=true;
            mem<int>(Rva::Stage)=requested.stage-1;
            originalTransition(p,0x16,practiceTransitionMode);
        } else field<int>(p,0x9638)=0;
        return 1;
    }
    if(confirmNativePractice && field<int>(p,0x168b0)==0x18 && field<int>(p,0x9638)>14) {
        confirmNativePractice=false;
        auto input=mem<uint16_t>(Rva::Input),previous=mem<uint16_t>(Rva::PreviousInput);
        mem<uint16_t>(Rva::Input)=0x100;mem<uint16_t>(Rva::PreviousInput)=0;
        auto result=originalMenu(p);
        mem<uint16_t>(Rva::Input)=input;mem<uint16_t>(Rva::PreviousInput)=previous;
        return result;
    }
    if(field<int>(p,0x168b0)==3) {
        armed=false;confirmNativePractice=false;
        active=false;retryPending=false;
        if(speedOwned){speed(originalFps);speedOwned=false;}
    }
    return originalMenu(p);
}

DWORD WINAPI start(void*) {
    diagnostics=DiagEnabled();
    imageBase=base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(HANDLE options=OpenFileMappingW(FILE_MAP_READ,FALSE,launchName(GetCurrentProcessId()).c_str())) {
        if(auto view=static_cast<const LaunchOptions*>(MapViewOfFile(options,FILE_MAP_READ,0,0,sizeof(LaunchOptions)))) {
            if(view->magic==Magic && view->version==Protocol &&
                (view->practice==0||view->practice==1) && (view->lowLatency==0||view->lowLatency==1) &&
                view->language>=0 && view->language<=2)launchOptions=*view;
            UnmapViewOfFile(view);
        }
        CloseHandle(options);
    }
    language.store(launchOptions.language);
    auto mn=mapName(GetCurrentProcessId());
    mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),mn.c_str());
    if(!mapping){DiagLogf("map create fail err=%lu",GetLastError());return 1;}
    shared=static_cast<Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
    mutex=CreateMutexW(nullptr,FALSE,mutexName(GetCurrentProcessId()).c_str());
    if(!shared||!mutex){DiagLogf("shared init fail view=%d mutex=%d",shared?1:0,mutex?1:0);return 2;}
    new(shared) Shared();
    shared->language=launchOptions.language;
    shared->practiceEnabled=launchOptions.practice;
    auto build=(launchOptions.build>=1&&launchOptions.build<=2)?(GameBuild)launchOptions.build:identifyBuild(executablePath());
    DiagLogf("build=%u",(unsigned)build);
    if(build==GameBuild::Unsupported) {status.error=1;message(L"游戏版本不匹配：当前 EXE 尚未适配");sync();return 3;}
    if(build==GameBuild::Steam25306795)Rva::UseSteam25306795();
    gGameBuild=build;
    if(MH_Initialize()!=MH_OK) {status.error=2;message(L"初始化钩子失败");sync();DiagLog("mh init fail");return 4;}
    struct Hook {uintptr_t rva;void* target;void** original;};
    Hook hooks[]={{Rva::GameInit,(void*)init,(void**)&originalInit},{Rva::GameUpdate,(void*)update,(void**)&originalUpdate},
      {Rva::PlayerUpdate,(void*)player,(void**)&originalPlayer},{Rva::EnemyUpdate,(void*)enemy,(void**)&originalEnemy},
      {Rva::EclUpdate,(void*)ecl,(void**)&originalEcl},{Rva::FinalSpellRage,(void*)finalPhase,(void**)&originalFinal},
      {Rva::MenuUpdate,(void*)menu,(void**)&originalMenu},{Rva::MenuTransition,(void*)transition,(void**)&originalTransition},
      {Rva::PlayBgm,(void*)playBgm,(void**)&originalBgm},
      {Rva::ResultInit,(void*)resultInit,(void**)&originalResultInit},
      {Rva::WriteFile,(void*)writeFile,(void**)&originalWrite},{Rva::SupervisorUpdate,(void*)supervisor,(void**)&originalSupervisor}};
    if(launchOptions.practice){
      for(auto& h:hooks)if(MH_CreateHook((void*)(base+h.rva),h.target,h.original)!=MH_OK){
        MH_Uninitialize();status.error=3;message(L"创建钩子失败，未应用补丁");sync();DiagLog("create hook fail");return 5;}
    }
    if(MH_EnableHook(MH_ALL_HOOKS)!=MH_OK){MH_Uninitialize();status.error=4;message(L"启用钩子失败");sync();DiagLog("enable fail");return 6;}
    status.ready=1;
    message(launchOptions.practice?L"已连接。从 Practice Start 选择单面后设置练习参数。":L"已连接，仅应用低延迟显示。");
    sync();
    HANDLE ot=CreateThread(nullptr,0,[](void* p)->DWORD{InstallOverlay(*(bool*)p);return 0;},new bool(launchOptions.lowLatency!=0),0,nullptr);
    if(ot)CloseHandle(ot); else DiagLog("overlay thread fail");
    return 0;
}
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(instance);DiagLog("dll attach");HANDLE t=CreateThread(nullptr,0,start,nullptr,0,nullptr);if(t)CloseHandle(t);}
    return TRUE;
}

namespace THPrac::TH06NC {
int MenuStage(){return mem<int>(Rva::Stage);}
int MenuDifficulty(){return mem<int>(Rva::Difficulty);}
int NativeRank(int difficulty){return mem<int>(Rva::NativeRanks+sizeof(int)*std::clamp(difficulty,0,4));}
int MenuShot(){return mem<uint8_t>(Rva::Character)*2+mem<uint8_t>(Rva::Shot);}
bool PracticeActive(){return active && mem<int>(Rva::CurrentState)==2 && mem<int>(Rva::NextState)==2;}
uint32_t PracticeFlags(){return playback?config.flags:requested.flags;}
int PracticeLanguage(){return language.load(std::memory_order_relaxed);}
void ChangeRequested(void(*change)(Settings&)) {
    if(!active||playback)return;
    auto lock=WaitForSingleObject(mutex,100);
    if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED){change(shared->settings);requested=shared->settings;ReleaseMutex(mutex);}
}
void RequestPracticeRestart(){ChangeRequested([](Settings& s){++s.restart;});}
int PracticeFps(){return requested.fps;}
void SetPracticeFps(int fps) {
    if(!active||playback)return;
    auto lock=WaitForSingleObject(mutex,100);
    if(lock==WAIT_OBJECT_0||lock==WAIT_ABANDONED){shared->settings.fps=std::clamp(fps,30,240);requested=shared->settings;ReleaseMutex(mutex);}
}
}
#endif
