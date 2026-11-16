#include "common.h"
#include "presentation.h"
#include <tlhelp32.h>
#include <shellapi.h>
#include "bridge_resource.h"
#include <shlobj.h>
#include <fstream>
namespace {
Shared* shared{}; HANDLE mapping{},mutex{}; DWORD targetPid{};
void log(const std::wstring& text) {
    static std::ofstream* f=nullptr;
    if(!f){ static std::ofstream fs; std::wstring p=directory(executablePath())+L"\\thprac-th06nc.log"; fs.open(std::string(p.begin(),p.end()), std::ios::app); f=&fs; }
    if(f&&f->is_open()){
        int n=WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,nullptr,0,nullptr,nullptr);
        std::string u(n>1?n-1:0,'\0'); if(n>1) WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,u.data(),n,nullptr,nullptr);
        (*f)<<u<<"\n"; f->flush();
    }
}
void failure(const std::wstring& text) { log(L"FAIL: "+text); MessageBoxW(nullptr,text.c_str(),L"thprac - th06nc",MB_ICONERROR); }
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
    dir = (n?std::wstring(tmp):directory(executablePath())+L"\\") + L"thprac-th06nc\\";
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
     std::wstring exe=directory(executablePath())+L"\\th06nc.exe";
     bool started=false;
     if(GetFileAttributesW(exe.c_str())!=INVALID_FILE_ATTRIBUTES) {
       STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{}; std::wstring cmd=L"\""+exe+L"\"";
       BOOL cp=CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,directory(executablePath()).c_str(),&si,&pi);
       log(std::wstring(L"startDirect(launch) exe=")+exe+L" cp="+std::to_wstring(cp)+L" err="+std::to_wstring(GetLastError()));
       if(cp){CloseHandle(pi.hThread);CloseHandle(pi.hProcess);started=true;}
     } else log(L"no th06nc.exe at "+exe);
     if(!started && (INT_PTR)ShellExecuteW(nullptr,L"open",L"steam://rungameid/4659620",nullptr,nullptr,SW_SHOWNORMAL)<=32) {failure(L"无法启动游戏。");return 1;}
   }
   for(int i=0;i<600&&!pid;++i) {pid=findGame();if(!pid)Sleep(100);}
 }
 if(!pid)pid=findGame();
 if(!pid) {
   std::wstring exe=directory(executablePath())+L"\\th06nc.exe";
   if(GetFileAttributesW(exe.c_str())!=INVALID_FILE_ATTRIBUTES) {
     STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{}; std::wstring cmd=L"\""+exe+L"\"";
     BOOL cp=CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,directory(executablePath()).c_str(),&si,&pi);
     log(std::wstring(L"startDirect exe=")+exe+L" cp="+std::to_wstring(cp)+L" err="+std::to_wstring(GetLastError()));
     if(cp){
       CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
       for(int i=0;i<600&&!pid;++i){pid=findGame();if(!pid)Sleep(100);}
     }
   } else log(L"no th06nc.exe at "+exe);
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
     MessageBoxW(nullptr,text,L"thprac - th06nc",MB_ICONWARNING);
   }
 }
 disconnect();return ok?0:3;
}
