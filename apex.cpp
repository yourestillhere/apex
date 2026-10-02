#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <xinput.h>
#include <d3d11.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")


static const COLORREF TRANSPARENT_COLOR = RGB(1, 1, 1);
static const float    CLEAR_COLOR[4] = {
    1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f, 1.0f
};


static int        g_ScreenW = 0;
static int        g_ScreenH = 0;
static const int  PANEL_W = 420;
static const int  PANEL_H = 440;

static float g_PanelX = -1.0f;
static float g_PanelY = -1.0f;

static int   g_ToggleVK = VK_INSERT;
static int   g_Slot1VK = '1';
static int   g_Slot2VK = '2';


enum class MouseMode : int
{
    SetCursorPos = 0,
    SendInputAbsolute = 1,
    SendInputRelative = 2,
};

static bool g_ShowModeChooser = true;


static const int CFG_NAME_MAX = 64;

struct Config {
    std::string name;
    bool        enabled;
    float       speed;
    float       amount;
    float       down;
    MouseMode   mouseMode;
};

static std::vector<Config> g_Configs;


static const int PROFILE_COUNT = 2;

struct Profile {
    int configIndex;
};

static Profile g_Profiles[PROFILE_COUNT];
static int     g_ActiveProfile = 0;


static bool g_ShowSaveAsPopup = false;
static char g_SaveAsName[CFG_NAME_MAX] = {};


struct Snowflake {
    float x, y;
    float vy;
    float sway;
    float phase;
    float radius;
    float alpha;
};

static std::vector<Snowflake> g_Snow;
static int         g_SnowCount = 600;
static const float SNOW_MIN_R = 1.0f;
static const float SNOW_MAX_R = 3.2f;

static float frand(float lo, float hi)
{
    return lo + (float)rand() / (float)RAND_MAX * (hi - lo);
}

void ResetSnowflake(Snowflake& s, float w, float h, bool anywhere)
{
    s.x = frand(0.0f, w);
    s.y = anywhere ? frand(0.0f, h) : frand(-40.0f, -5.0f);
    s.vy = frand(20.0f, 70.0f);
    s.sway = frand(4.0f, 18.0f);
    s.phase = frand(0.0f, 6.2831853f);
    s.radius = frand(SNOW_MIN_R, SNOW_MAX_R);
    s.alpha = frand(0.5f, 1.0f);
}

void InitSnow(float w, float h)
{
    g_Snow.resize(g_SnowCount);
    for (auto& s : g_Snow)
        ResetSnowflake(s, w, h, /*anywhere=*/true);
}


static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;


static HWND g_hWnd = nullptr;
static bool g_Running = true;
static bool g_OverlayVisible = true;


static POINT          g_OriginalPos = { 0, 0 };
static POINT          g_LastSetPos = { 0, 0 };
static bool           g_IsShaking = false;
static double         g_ShakeTime = 0.0;
static LARGE_INTEGER  g_Freq = {};
static LARGE_INTEGER  g_LastTime = {};

static float          g_DownAccum = 0.0f;
static const float    DOWN_RATE_HZ = 1.0f;


LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);
bool    CreateDeviceD3D(HWND hWnd);
void    CleanupDeviceD3D();
void    CreateRenderTarget();
void    CleanupRenderTarget();
void    LoadSettings();
void    SaveSettings();
void    SavePanelPosition(float x, float y);
void    CreateDefaultConfig();
Config& ActiveCfg();
bool    IsTriggerHeld();
void    StartShaking();
void    StopShaking();
void    PerformShake(double deltaTime);
void    MoveMouseTo(int x, int y);
void    PositionOverlay(HWND hWnd);
void    ApplyTransparency(HWND hWnd);
void    UpdateAndDrawSnow(float dt, ImDrawList* dl);
void    DrawModeChooser();
void    DrawKeybindButton(const char* label, int* vk);
const char* KeyName(int vk);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);


Config& ActiveCfg()
{
    if (g_Configs.empty())
        CreateDefaultConfig();

    int idx = g_Profiles[g_ActiveProfile].configIndex;
    if (idx < 0 || idx >= (int)g_Configs.size())
        idx = 0;
    return g_Configs[idx];
}

void CreateDefaultConfig()
{
    Config c;
    c.name = "Default";
    c.enabled = true;
    c.speed = 5.0f;
    c.amount = 10.0f;
    c.down = 0.0f;
    c.mouseMode = MouseMode::SetCursorPos;
    g_Configs.push_back(c);
}


int WINAPI wWinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_     PWSTR     pCmdLine,
    _In_     int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(pCmdLine);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    srand((unsigned)GetTickCount64());

    QueryPerformanceFrequency(&g_Freq);
    QueryPerformanceCounter(&g_LastTime);

    g_ScreenW = GetSystemMetrics(SM_CXSCREEN);
    g_ScreenH = GetSystemMetrics(SM_CYSCREEN);
    if (g_ScreenW <= 0) g_ScreenW = 1920;
    if (g_ScreenH <= 0) g_ScreenH = 1080;

    if (g_PanelX < 0) g_PanelX = (float)(g_ScreenW - PANEL_W - 12);
    if (g_PanelY < 0) g_PanelY = 12.0f;

    LoadSettings();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"MouseShakerOverlay";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"MouseShakerOverlay",
        L"Apex",
        WS_POPUP,
        0, 0, g_ScreenW, g_ScreenH,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd)
        return 0;

    ApplyTransparency(g_hWnd);

    if (!CreateDeviceD3D(g_hWnd))
    {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    PositionOverlay(g_hWnd);
    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 8.0f;
    ImGui::GetStyle().WindowBorderSize = 0.0f;
    ImGui::GetStyle().FrameRounding = 4.0f;

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    InitSnow((float)g_ScreenW, (float)g_ScreenH);

    while (g_Running)
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                g_Running = false;
        }
        if (!g_Running)
            break;

        static bool prevToggle = false;
        static bool prevSlot1 = false;
        static bool prevSlot2 = false;

        bool nowToggle = (GetAsyncKeyState(g_ToggleVK) & 0x8000) != 0;
        bool nowSlot1 = (GetAsyncKeyState(g_Slot1VK) & 0x8000) != 0;
        bool nowSlot2 = (GetAsyncKeyState(g_Slot2VK) & 0x8000) != 0;

        if (nowToggle && !prevToggle)
        {
            g_OverlayVisible = !g_OverlayVisible;
            ShowWindow(g_hWnd, g_OverlayVisible ? SW_SHOW : SW_HIDE);
            if (g_OverlayVisible)
                SetForegroundWindow(g_hWnd);
        }
        if (nowSlot1 && !prevSlot1)
        {
            g_ActiveProfile = 0;
            g_DownAccum = 0.0f;
            SaveSettings();
        }
        if (nowSlot2 && !prevSlot2)
        {
            g_ActiveProfile = 1;
            g_DownAccum = 0.0f;
            SaveSettings();
        }

        prevToggle = nowToggle;
        prevSlot1 = nowSlot1;
        prevSlot2 = nowSlot2;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double delta = double(now.QuadPart - g_LastTime.QuadPart) / double(g_Freq.QuadPart);
        g_LastTime = now;
        float fdelta = (float)delta;

        Config& ac = ActiveCfg();
        if (!g_ShowModeChooser && ac.enabled && IsTriggerHeld())
        {
            if (!g_IsShaking) StartShaking();
            PerformShake(delta);
        }
        else if (g_IsShaking)
        {
            StopShaking();
        }

        if (!g_OverlayVisible)
        {
            Sleep(1);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        UpdateAndDrawSnow(fdelta, bg);

        if (g_ShowModeChooser)
        {
            DrawModeChooser();
        }
        else
        {
            ImGui::SetNextWindowPos(ImVec2(g_PanelX, g_PanelY), ImGuiCond_Once);
            ImGui::SetNextWindowSize(ImVec2((float)PANEL_W, (float)PANEL_H), ImGuiCond_Once);

            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.04f, 0.04f, 0.04f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.10f, 0.10f, 0.95f));

            ImGui::Begin("Apex##overlay", nullptr,
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings);

            ImVec2 curPos = ImGui::GetWindowPos();
            if (curPos.x != g_PanelX || curPos.y != g_PanelY)
            {
                g_PanelX = curPos.x;
                g_PanelY = curPos.y;
                SavePanelPosition(g_PanelX, g_PanelY);
            }

            if (ImGui::BeginTabBar("##profiles", ImGuiTabBarFlags_None))
            {
                for (int i = 0; i < PROFILE_COUNT; ++i)
                {
                    char tabLabel[32];
                    snprintf(tabLabel, sizeof(tabLabel), "Profile %d###prof%d", i + 1, i);

                    bool isOpen = true;
                    if (ImGui::BeginTabItem(tabLabel, &isOpen, ImGuiTabItemFlags_None))
                    {
                        if (i == g_ActiveProfile)
                            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "ACTIVE");
                        else
                        {
                            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "INACTIVE");
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Activate"))
                            {
                                g_ActiveProfile = i;
                                g_DownAccum = 0.0f;
                                SaveSettings();
                            }
                        }

                        ImGui::Separator();

                        ImGui::Text("Config:");

                        int cfgIdx = g_Profiles[i].configIndex;
                        if (cfgIdx < 0 || cfgIdx >= (int)g_Configs.size()) cfgIdx = 0;

                        const char* preview = g_Configs.empty()
                            ? "(none)"
                            : g_Configs[cfgIdx].name.c_str();

                        ImGui::SetNextItemWidth(-1.0f);
                        char comboId[32];
                        snprintf(comboId, sizeof(comboId), "##cfgcombo%d", i);

                        if (ImGui::BeginCombo(comboId, preview))
                        {
                            for (int k = 0; k < (int)g_Configs.size(); ++k)
                            {
                                bool selected = (k == cfgIdx);
                                if (ImGui::Selectable(g_Configs[k].name.c_str(), selected))
                                {
                                    g_Profiles[i].configIndex = k;
                                    SaveSettings();
                                }
                                if (selected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::Separator();

                        if (cfgIdx >= 0 && cfgIdx < (int)g_Configs.size())
                        {
                            Config& cfg = g_Configs[cfgIdx];

                            if (ImGui::Checkbox("Enable", &cfg.enabled))                                 SaveSettings();
                            if (ImGui::SliderFloat("Speed", &cfg.speed, 0.5f, 20.0f, "%.1f osc/s"))   SaveSettings();
                            if (ImGui::SliderFloat("Amount", &cfg.amount, 1.0f, 50.0f, "%.0f px"))       SaveSettings();
                            if (ImGui::SliderFloat("Down", &cfg.down, -200.0f, 200.0f, "%.0f px/s"))   SaveSettings();

                            const char* modeName =
                                (cfg.mouseMode == MouseMode::SetCursorPos) ? "SetCursorPos" :
                                (cfg.mouseMode == MouseMode::SendInputAbsolute) ? "SendInput Absolute" :
                                "SendInput Relative";
                            ImGui::Text("Mouse Mode: %s", modeName);

                            ImGui::Separator();

                            if (ImGui::Button("Save as new...", ImVec2(180, 0)))
                            {
                                g_ShowSaveAsPopup = true;
                                g_SaveAsName[0] = 0;
                                ImGui::OpenPopup("Save Config");
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Delete config", ImVec2(160, 0)))
                            {
                                if (g_Configs.size() > 1)
                                {
                                    g_Configs.erase(g_Configs.begin() + cfgIdx);

                                    for (int p = 0; p < PROFILE_COUNT; ++p)
                                    {
                                        if (g_Profiles[p].configIndex >= (int)g_Configs.size())
                                            g_Profiles[p].configIndex = (int)g_Configs.size() - 1;
                                        if (g_Profiles[p].configIndex < 0)
                                            g_Profiles[p].configIndex = 0;
                                    }
                                    SaveSettings();
                                }
                            }
                        }

                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }

            if (ImGui::BeginPopupModal("Save Config", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Name for this config:");
                ImGui::InputText("##name", g_SaveAsName, CFG_NAME_MAX);
                ImGui::Spacing();

                if (ImGui::Button("Save", ImVec2(100, 0)))
                {
                    if (strlen(g_SaveAsName) > 0)
                    {
                        Config c = ActiveCfg();
                        c.name = g_SaveAsName;
                        g_Configs.push_back(c);
                        g_Profiles[g_ActiveProfile].configIndex = (int)g_Configs.size() - 1;
                        SaveSettings();
                    }
                    g_ShowSaveAsPopup = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100, 0)))
                {
                    g_ShowSaveAsPopup = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Separator();

            if (ImGui::CollapsingHeader("Keybinds"))
            {
                DrawKeybindButton("Show / hide", &g_ToggleVK);
                DrawKeybindButton("Profile 1", &g_Slot1VK);
                DrawKeybindButton("Profile 2", &g_Slot2VK);
                ImGui::TextDisabled("Click a key, then press the new key.");
            }

            ImGui::Separator();
            ImGui::TextWrapped("Hold LMB + RMB (or LT + RT) to shake.");
            ImGui::TextWrapped("Press INSERT to hide the overlay before playing.");
            ImGui::TextDisabled("Drag this panel by its title bar.");

            ImGui::End();

            ImGui::PopStyleColor(3);
        }

        ImGui::Render();

        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, CLEAR_COLOR);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    SaveSettings();
    if (g_IsShaking) StopShaking();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(g_hWnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}


void DrawModeChooser()
{
    float w = 380.0f;
    float h = 380.0f;
    ImGui::SetNextWindowPos(
        ImVec2((g_ScreenW - w) * 0.5f, (g_ScreenH - h) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::Begin("##modechooser", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.06f, 0.95f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::BeginChild("##chooserbody", ImVec2(0, 0), true);

    ImGui::Text("Choose Mouse Mode");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped(
        "How should the shaker move the cursor? "
        "You'll only be asked once per launch. To change it, restart the app.");

    ImGui::Separator();
    ImGui::Spacing();

    Config& ac = ActiveCfg();
    bool picked = false;

    if (ImGui::Button("SetCursorPos", ImVec2(-1, 0)))
    {
        ac.mouseMode = MouseMode::SetCursorPos;
        picked = true;
    }
    ImGui::TextWrapped("Teleports the cursor directly. Instant, but raw-input games ignore it.");

    ImGui::Spacing();

    if (ImGui::Button("SendInput - Absolute", ImVec2(-1, 0)))
    {
        ac.mouseMode = MouseMode::SendInputAbsolute;
        picked = true;
    }
    ImGui::TextWrapped("Goes through the OS input stack. Works in most games.");

    ImGui::Spacing();

    if (ImGui::Button("SendInput - Relative", ImVec2(-1, 0)))
    {
        ac.mouseMode = MouseMode::SendInputRelative;
        picked = true;
    }
    ImGui::TextWrapped("Sends delta moves, like a real mouse. Best for FPS/raw-input games.");

    ImGui::Spacing();
    ImGui::Separator();

    if (picked)
    {
        g_ShowModeChooser = false;
        g_DownAccum = 0.0f;
        SaveSettings();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleColor();
}


static int* g_RebindingPtr = nullptr;

const char* KeyName(int vk)
{
    static char buf[32];

    if (vk >= '0' && vk <= '9') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= 'A' && vk <= 'Z') { buf[0] = (char)vk; buf[1] = 0; return buf; }

    switch (vk)
    {
    case VK_INSERT:    return "INSERT";
    case VK_DELETE:    return "DELETE";
    case VK_HOME:      return "HOME";
    case VK_END:       return "END";
    case VK_PRIOR:     return "PAGE UP";
    case VK_NEXT:      return "PAGE DOWN";
    case VK_ESCAPE:    return "ESCAPE";
    case VK_TAB:       return "TAB";
    case VK_BACK:      return "BACKSPACE";
    case VK_RETURN:    return "ENTER";
    case VK_SPACE:     return "SPACE";
    case VK_F1:        return "F1";
    case VK_F2:        return "F2";
    case VK_F3:        return "F3";
    case VK_F4:        return "F4";
    case VK_F5:        return "F5";
    case VK_F6:        return "F6";
    case VK_F7:        return "F7";
    case VK_F8:        return "F8";
    case VK_F9:        return "F9";
    case VK_F10:       return "F10";
    case VK_F11:       return "F11";
    case VK_F12:       return "F12";
    case VK_OEM_3:     return "`";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PLUS:  return "=";
    case VK_OEM_4:     return "[";
    case VK_OEM_6:     return "]";
    case VK_OEM_5:     return "\\";
    case VK_OEM_1:     return ";";
    case VK_OEM_7:     return "'";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_PERIOD:return ".";
    case VK_OEM_2:     return "/";
    default:
        snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
        return buf;
    }
}

void DrawKeybindButton(const char* label, int* vk)
{
    ImGui::PushID(vk);

    ImGui::Text("%s:", label);
    ImGui::SameLine(140);

    bool active = (g_RebindingPtr == vk);

    char btnText[64];
    snprintf(btnText, sizeof(btnText), "%s", active ? "..." : KeyName(*vk));

    if (ImGui::Button(btnText, ImVec2(140, 0)))
    {
        g_RebindingPtr = vk;
    }

    if (active)
    {
        for (int v = 0x08; v <= 0xFE; ++v)
        {
            if (v == VK_LBUTTON || v == VK_RBUTTON || v == VK_MBUTTON) continue;
            if (v == VK_SHIFT || v == VK_CONTROL || v == VK_MENU) continue;

            if (GetAsyncKeyState(v) & 0x8000)
            {
                *vk = v;
                g_RebindingPtr = nullptr;
                SaveSettings();
                break;
            }
        }
    }

    ImGui::PopID();
}


LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}


void ApplyTransparency(HWND hWnd)
{
    LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    ex |= WS_EX_LAYERED;
    SetWindowLongPtrW(hWnd, GWL_EXSTYLE, ex);
    SetLayeredWindowAttributes(hWnd, TRANSPARENT_COLOR, 0, LWA_COLORKEY);
}


void PositionOverlay(HWND hWnd)
{
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, g_ScreenW, g_ScreenH,
        SWP_SHOWWINDOW);
}

bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = g_ScreenW;
    sd.BufferDesc.Height = g_ScreenH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer)
    {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView)
    {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static const wchar_t* REG_KEY = L"Software\\MouseShaker";

void SavePanelPosition(float x, float y)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD vx = (DWORD)(int)x;
        DWORD vy = (DWORD)(int)y;
        RegSetValueExW(hKey, L"PanelX", 0, REG_DWORD, (LPBYTE)&vx, sizeof(vx));
        RegSetValueExW(hKey, L"PanelY", 0, REG_DWORD, (LPBYTE)&vy, sizeof(vy));
        RegCloseKey(hKey);
    }
}

static void SaveConfigsAndProfiles(HKEY hRoot)
{
    RegDeleteTreeW(hRoot, L"Configs");
    RegDeleteTreeW(hRoot, L"Profiles");

    HKEY hCfg;
    if (RegCreateKeyExW(hRoot, L"Configs", 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hCfg, nullptr) == ERROR_SUCCESS)
    {
        DWORD count = (DWORD)g_Configs.size();
        RegSetValueExW(hCfg, L"Count", 0, REG_DWORD, (LPBYTE)&count, sizeof(count));

        for (int i = 0; i < (int)g_Configs.size(); ++i)
        {
            wchar_t sub[32]; swprintf_s(sub, L"Cfg%d", i);
            HKEY hSub;
            if (RegCreateKeyExW(hCfg, sub, 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hSub, nullptr) != ERROR_SUCCESS)
                continue;

            const Config& c = g_Configs[i];

            wchar_t nameBuf[CFG_NAME_MAX];
            MultiByteToWideChar(CP_UTF8, 0, c.name.c_str(), -1, nameBuf, CFG_NAME_MAX);
            RegSetValueExW(hSub, L"Name", 0, REG_SZ, (LPBYTE)nameBuf,
                (DWORD)((wcslen(nameBuf) + 1) * sizeof(wchar_t)));

            DWORD v;
            v = c.enabled ? 1 : 0;             RegSetValueExW(hSub, L"Enabled", 0, REG_DWORD, (LPBYTE)&v, sizeof(v));
            v = (DWORD)(c.speed * 100.0f);     RegSetValueExW(hSub, L"Speed", 0, REG_DWORD, (LPBYTE)&v, sizeof(v));
            v = (DWORD)c.amount;               RegSetValueExW(hSub, L"Amount", 0, REG_DWORD, (LPBYTE)&v, sizeof(v));
            v = (DWORD)(int)c.down;            RegSetValueExW(hSub, L"Down", 0, REG_DWORD, (LPBYTE)&v, sizeof(v));
            v = (DWORD)c.mouseMode;            RegSetValueExW(hSub, L"MouseMode", 0, REG_DWORD, (LPBYTE)&v, sizeof(v));

            RegCloseKey(hSub);
        }
        RegCloseKey(hCfg);
    }

    HKEY hProf;
    if (RegCreateKeyExW(hRoot, L"Profiles", 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hProf, nullptr) == ERROR_SUCCESS)
    {
        DWORD active = (DWORD)g_ActiveProfile;
        RegSetValueExW(hProf, L"Active", 0, REG_DWORD, (LPBYTE)&active, sizeof(active));

        for (int i = 0; i < PROFILE_COUNT; ++i)
        {
            wchar_t sub[32]; swprintf_s(sub, L"P%d", i);
            HKEY hSub;
            if (RegCreateKeyExW(hProf, sub, 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hSub, nullptr) != ERROR_SUCCESS)
                continue;

            DWORD idx = (DWORD)g_Profiles[i].configIndex;
            RegSetValueExW(hSub, L"ConfigIndex", 0, REG_DWORD, (LPBYTE)&idx, sizeof(idx));
            RegCloseKey(hSub);
        }
        RegCloseKey(hProf);
    }
}

static void LoadConfigsAndProfiles(HKEY hRoot)
{
    g_Configs.clear();
    g_Profiles[0].configIndex = 0;
    g_Profiles[1].configIndex = 0;
    g_ActiveProfile = 0;

    HKEY hCfg;
    if (RegOpenKeyExW(hRoot, L"Configs", 0, KEY_READ, &hCfg) == ERROR_SUCCESS)
    {
        DWORD count = 0, sz = sizeof(count);
        RegQueryValueExW(hCfg, L"Count", nullptr, nullptr, (LPBYTE)&count, &sz);

        for (DWORD i = 0; i < count; ++i)
        {
            wchar_t sub[32]; swprintf_s(sub, L"Cfg%u", i);
            HKEY hSub;
            if (RegOpenKeyExW(hCfg, sub, 0, KEY_READ, &hSub) != ERROR_SUCCESS) continue;

            Config c;
            c.enabled = true;
            c.speed = 5.0f;
            c.amount = 10.0f;
            c.down = 0.0f;
            c.mouseMode = MouseMode::SetCursorPos;

            wchar_t nameBuf[CFG_NAME_MAX];
            DWORD nameLen = sizeof(nameBuf);
            DWORD type = 0;
            if (RegQueryValueExW(hSub, L"Name", nullptr, &type, (LPBYTE)nameBuf, &nameLen) == ERROR_SUCCESS
                && type == REG_SZ)
            {
                char narrow[CFG_NAME_MAX];
                WideCharToMultiByte(CP_UTF8, 0, nameBuf, -1, narrow, CFG_NAME_MAX, nullptr, nullptr);
                c.name = narrow;
            }
            else c.name = "Config";

            DWORD v, sz2;
            sz2 = sizeof(v); if (RegQueryValueExW(hSub, L"Enabled", nullptr, nullptr, (LPBYTE)&v, &sz2) == ERROR_SUCCESS) c.enabled = (v != 0);
            sz2 = sizeof(v); if (RegQueryValueExW(hSub, L"Speed", nullptr, nullptr, (LPBYTE)&v, &sz2) == ERROR_SUCCESS) c.speed = v / 100.0f;
            sz2 = sizeof(v); if (RegQueryValueExW(hSub, L"Amount", nullptr, nullptr, (LPBYTE)&v, &sz2) == ERROR_SUCCESS) c.amount = (float)v;
            sz2 = sizeof(v); if (RegQueryValueExW(hSub, L"Down", nullptr, nullptr, (LPBYTE)&v, &sz2) == ERROR_SUCCESS) c.down = (float)((int)v);
            sz2 = sizeof(v); if (RegQueryValueExW(hSub, L"MouseMode", nullptr, nullptr, (LPBYTE)&v, &sz2) == ERROR_SUCCESS) c.mouseMode = (MouseMode)v;

            RegCloseKey(hSub);
            g_Configs.push_back(c);
        }
        RegCloseKey(hCfg);
    }

    if (g_Configs.empty())
        CreateDefaultConfig();

    HKEY hProf;
    if (RegOpenKeyExW(hRoot, L"Profiles", 0, KEY_READ, &hProf) == ERROR_SUCCESS)
    {
        DWORD active = 0, sz = sizeof(active);
        RegQueryValueExW(hProf, L"Active", nullptr, nullptr, (LPBYTE)&active, &sz);
        g_ActiveProfile = (active < PROFILE_COUNT) ? (int)active : 0;

        for (int i = 0; i < PROFILE_COUNT; ++i)
        {
            wchar_t sub[32]; swprintf_s(sub, L"P%d", i);
            HKEY hSub;
            if (RegOpenKeyExW(hProf, sub, 0, KEY_READ, &hSub) != ERROR_SUCCESS) continue;

            DWORD idx = 0, sz2 = sizeof(idx);
            RegQueryValueExW(hSub, L"ConfigIndex", nullptr, nullptr, (LPBYTE)&idx, &sz2);
            g_Profiles[i].configIndex = (int)idx;
            RegCloseKey(hSub);
        }
        RegCloseKey(hProf);
    }

    for (int i = 0; i < PROFILE_COUNT; ++i)
    {
        if (g_Profiles[i].configIndex < 0) g_Profiles[i].configIndex = 0;
        if (g_Profiles[i].configIndex >= (int)g_Configs.size())
            g_Profiles[i].configIndex = (int)g_Configs.size() - 1;
    }
}

void LoadSettings()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD size = sizeof(DWORD);
        DWORD val = 0;

        if (RegQueryValueExW(hKey, L"VK_Toggle", nullptr, nullptr, (LPBYTE)&val, &size) == ERROR_SUCCESS)
            g_ToggleVK = (int)val;
        if (RegQueryValueExW(hKey, L"VK_Slot1", nullptr, nullptr, (LPBYTE)&val, &size) == ERROR_SUCCESS)
            g_Slot1VK = (int)val;
        if (RegQueryValueExW(hKey, L"VK_Slot2", nullptr, nullptr, (LPBYTE)&val, &size) == ERROR_SUCCESS)
            g_Slot2VK = (int)val;

        if (RegQueryValueExW(hKey, L"PanelX", nullptr, nullptr, (LPBYTE)&val, &size) == ERROR_SUCCESS)
            g_PanelX = (float)((int)val);
        if (RegQueryValueExW(hKey, L"PanelY", nullptr, nullptr, (LPBYTE)&val, &size) == ERROR_SUCCESS)
            g_PanelY = (float)((int)val);

        LoadConfigsAndProfiles(hKey);
        RegCloseKey(hKey);
    }
    else
    {
        CreateDefaultConfig();
        g_Profiles[0].configIndex = 0;
        g_Profiles[1].configIndex = 0;
        g_ActiveProfile = 0;
    }
}

void SaveSettings()
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD val;

        val = (DWORD)g_ToggleVK; RegSetValueExW(hKey, L"VK_Toggle", 0, REG_DWORD, (LPBYTE)&val, sizeof(val));
        val = (DWORD)g_Slot1VK;  RegSetValueExW(hKey, L"VK_Slot1", 0, REG_DWORD, (LPBYTE)&val, sizeof(val));
        val = (DWORD)g_Slot2VK;  RegSetValueExW(hKey, L"VK_Slot2", 0, REG_DWORD, (LPBYTE)&val, sizeof(val));

        SaveConfigsAndProfiles(hKey);
        RegCloseKey(hKey);
    }
}

bool IsTriggerHeld()
{
    bool leftMouse = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rightMouse = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    bool mouseCombo = leftMouse && rightMouse;

    bool leftTrigger = false;
    bool rightTrigger = false;

    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
    {
        XINPUT_STATE state = {};
        if (XInputGetState(i, &state) == ERROR_SUCCESS)
        {
            if (state.Gamepad.bLeftTrigger > 30) leftTrigger = true;
            if (state.Gamepad.bRightTrigger > 30) rightTrigger = true;
        }
    }

    return mouseCombo || (leftTrigger && rightTrigger);
}

void MoveMouseTo(int x, int y)
{
    MouseMode mode = ActiveCfg().mouseMode;

    switch (mode)
    {
    case MouseMode::SetCursorPos:
        SetCursorPos(x, y);
        break;

    case MouseMode::SendInputAbsolute:
    {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        if (sw <= 0) sw = 1;
        if (sh <= 0) sh = 1;

        double nx = (double)x / (double)(sw - 1) * 65535.0;
        double ny = (double)y / (double)(sh - 1) * 65535.0;

        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dx = (LONG)(nx + 0.5);
        in.mi.dy = (LONG)(ny + 0.5);
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &in, sizeof(INPUT));
        break;
    }

    case MouseMode::SendInputRelative:
    {
        int dx = x - g_LastSetPos.x;
        int dy = y - g_LastSetPos.y;

        if (dx != 0 || dy != 0)
        {
            INPUT in = {};
            in.type = INPUT_MOUSE;
            in.mi.dx = dx;
            in.mi.dy = dy;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &in, sizeof(INPUT));
        }
        break;
    }
    }

    g_LastSetPos.x = x;
    g_LastSetPos.y = y;
}

void StartShaking()
{
    g_IsShaking = true;
    g_ShakeTime = 0.0;
    g_DownAccum = 0.0f;
    GetCursorPos(&g_OriginalPos);
    g_LastSetPos = g_OriginalPos;
}

void StopShaking()
{
    g_IsShaking = false;
    MoveMouseTo(g_OriginalPos.x, g_OriginalPos.y);
}

void PerformShake(double deltaTime)
{
    const double PI = 3.14159265358979323846;
    Config& ac = ActiveCfg();

    g_DownAccum += ac.down * DOWN_RATE_HZ * (float)deltaTime;

    g_ShakeTime += deltaTime * ac.speed * 2.0 * PI;

    double offsetX = sin(g_ShakeTime) * ac.amount;

    int newX = g_OriginalPos.x + (int)offsetX;
    int newY = g_OriginalPos.y + (int)g_DownAccum;
    MoveMouseTo(newX, newY);
}

void UpdateAndDrawSnow(float dt, ImDrawList* dl)
{
    ImVec2 vp = ImGui::GetIO().DisplaySize;
    float w = vp.x;
    float h = vp.y;

    if (w < 1.0f || h < 1.0f)
        return;

    static float lastW = 0.0f;
    static float lastH = 0.0f;
    if (fabsf(w - lastW) > 1.0f || fabsf(h - lastH) > 1.0f)
    {
        InitSnow(w, h);
        lastW = w;
        lastH = h;
    }

    static float t = 0.0f;
    t += dt;

    for (auto& s : g_Snow)
    {
        s.y += s.vy * dt;
        float xOffset = sinf(t * 1.5f + s.phase) * s.sway;

        if (s.y - s.radius > h + 4.0f)
        {
            ResetSnowflake(s, w, h, /*anywhere=*/false);
            continue;
        }

        float drawX = s.x + xOffset;
        if (drawX < -8.0f) drawX += w + 16.0f;
        if (drawX > w + 8.0f) drawX -= w + 16.0f;

        ImU32 col = IM_COL32(255, 255, 255, (int)(s.alpha * 220.0f));
        ImU32 halo = IM_COL32(255, 255, 255, (int)(s.alpha * 70.0f));

        dl->AddCircleFilled(ImVec2(drawX, s.y), s.radius + 1.2f, halo, 12);
        dl->AddCircleFilled(ImVec2(drawX, s.y), s.radius, col, 12);
    }
}
