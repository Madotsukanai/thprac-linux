#include "module.h"
#include "presentation.h"
#include "spell_names.h"
#include "section_menu.h"
#include "thprac_gui_components.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "MinHook.h"
#include <d3d11.h>
#include <memory>
#include <cstdio>

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
#include "practice_ui.inl"
THGuiPrac& PracticeUi();
#include "pause_ui.inl"
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
    auto rlogLocal=[](const char* msg){FILE* f=fopen("thprac-linux.log","ab"); if(f){fputs("[dll] ",f);fputs(msg,f);fputs("\n",f);fclose(f);}};
    auto entry = (*reinterpret_cast<void***>(swap))[8];
    MH_STATUS st=MH_CreateHook(entry, reinterpret_cast<void*>(Present), reinterpret_cast<void**>(&originalPresent));
    if (st == MH_OK) {
        if (overlayLowLatency) InstallLowLatency((*reinterpret_cast<void***>(swap))[13]);
        MH_EnableHook(entry);
        overlayInstalled = true;
        rlogLocal("overlay: present hooked");
    } else { char buf[64]; snprintf(buf,sizeof(buf),"overlay: present hook fail %d",st); rlogLocal(buf); }
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
    auto rlogLocal=[](const char* msg){FILE* f=fopen("thprac-linux.log","ab"); if(f){fputs("[dll] ",f);fputs(msg,f);fputs("\n",f);fclose(f);}};
    rlogLocal("overlay: fallback dummy swap chain");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"ThpracNcDummyWindow";
    RegisterClassExW(&wc);
    HWND hwnd=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
    if(!hwnd){rlogLocal("overlay: dummy window fail");return false;}
    rlogLocal("overlay: dummy window ok");
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount=1; sd.BufferDesc.Width=64; sd.BufferDesc.Height=64;
    sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow=hwnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap{}; ID3D11Device* dev{}; ID3D11DeviceContext* ctx{}; D3D_FEATURE_LEVEL level{};
    // Call the trampoline so HookedCreateDevice is not re-entered.
    HRESULT hr=originalCreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&swap,&dev,&level,&ctx);
    char buf[64]; snprintf(buf,sizeof(buf),"overlay: dummy create device hr=0x%08lx",(unsigned long)hr); rlogLocal(buf);
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
    auto rlogLocal=[](const char* msg){FILE* f=fopen("thprac-linux.log","ab"); if(f){fputs("[dll] ",f);fputs(msg,f);fputs("\n",f);fclose(f);}};
    rlogLocal("overlay: begin");
    overlayLowLatency = lowLatency;
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    for(int i=0;i<600 && !d3d11;i++){Sleep(50); d3d11=GetModuleHandleW(L"d3d11.dll");}
    if (!d3d11) { rlogLocal("overlay: d3d11 not loaded"); return false; }
    rlogLocal("overlay: d3d11 loaded");
    auto proc = GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    if (!proc) { rlogLocal("overlay: no proc"); return false; }
    MH_STATUS st=MH_CreateHook(reinterpret_cast<void*>(proc), reinterpret_cast<void*>(HookedCreateDevice), reinterpret_cast<void**>(&originalCreateDevice));
    if (st != MH_OK) { char buf[64]; snprintf(buf,sizeof(buf),"overlay: createhook fail %d",st); rlogLocal(buf); return false; }
    st=MH_EnableHook(reinterpret_cast<void*>(proc));
    if (st != MH_OK) { char buf[64]; snprintf(buf,sizeof(buf),"overlay: enable fail %d",st); rlogLocal(buf); return false; }
    rlogLocal("overlay: hooked create");
    // Give the game time to call the hooked function itself; if it did not, hook Present directly.
    for(int i=0;i<60 && !overlayInstalled;++i)Sleep(50);
    if(!overlayInstalled)InstallPresentHookFromDummy();
    return true;
}
}
