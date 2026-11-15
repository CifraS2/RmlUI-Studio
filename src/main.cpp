#include "dx11_renderer.h"
#include "rml_doc.h"
#include "rml_engine.h"
#include "studio_ui.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <windows.h>
#include <windowsx.h>

#pragma comment(lib, "dwmapi.lib")

namespace {

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
bool g_SwapChainOccluded = false;
UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
int g_ClientWidth = 1440, g_ClientHeight = 860;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
StudioUI* g_studio = nullptr;

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
    );
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, featureLevelArray, 2,
            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
        );
    }
    if (res != S_OK) return false;
    CreateRenderTarget();
    return true;
}

constexpr UINT WM_APP_TOGGLE_FULLSCREEN = WM_APP + 101;
bool g_isFullscreen = false;
WINDOWPLACEMENT g_prevPlacement{sizeof(g_prevPlacement)};
LONG_PTR g_prevStyle = WS_OVERLAPPEDWINDOW;

void ToggleFullscreen(HWND hwnd) {
    if (!hwnd) return;
    if (!g_isFullscreen) {
        g_prevPlacement.length = sizeof(g_prevPlacement);
        GetWindowPlacement(hwnd, &g_prevPlacement);
        g_prevStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);

        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, (g_prevStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            SetWindowPos(
                hwnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED
            );
            g_isFullscreen = true;
        }
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_prevStyle | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_prevPlacement);
        SetWindowPos(
            hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED
        );
        g_isFullscreen = false;
    }
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_TOGGLE_FULLSCREEN:
            ToggleFullscreen(hWnd);
            return 0;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth = LOWORD(lParam);
            g_ResizeHeight = HIWORD(lParam);
            if (g_ResizeWidth > 0 && g_ResizeHeight > 0) {
                g_ClientWidth = (int)g_ResizeWidth;
                g_ClientHeight = (int)g_ResizeHeight;
                if (g_studio) g_studio->OnResize(g_ClientWidth, g_ClientHeight);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (g_studio) g_studio->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONDOWN:
            if (g_studio) {
                g_studio->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                g_studio->OnMouseButton(0, true);
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_studio) {
                g_studio->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                g_studio->OnMouseButton(0, false);
            }
            return 0;
        case WM_RBUTTONDOWN:
            if (g_studio) {
                g_studio->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                g_studio->OnMouseButton(1, true);
            }
            return 0;
        case WM_RBUTTONUP:
            if (g_studio) {
                g_studio->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                g_studio->OnMouseButton(1, false);
            }
            return 0;
        case WM_MBUTTONDOWN:
            if (g_studio) {
                g_studio->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                g_studio->OnMouseButton(2, true);
            }
            return 0;
        case WM_MBUTTONUP:
            if (g_studio) {
                g_studio->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                g_studio->OnMouseButton(2, false);
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (g_studio) {
                float delta = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
                g_studio->OnMouseWheel(delta);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_F11 && ((lParam & (1LL << 30)) == 0)) {
                ToggleFullscreen(hWnd);
                return 0;
            }
            if (g_studio) g_studio->OnKey((UINT)wParam, true, wParam, lParam);
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (g_studio) g_studio->OnKey((UINT)wParam, false, wParam, lParam);
            break;
        case WM_CHAR:
            if (g_studio) g_studio->OnChar((uint32_t)wParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();

    WNDCLASSEXW wc{
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr,
        LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr,
        L"RmlUIStudio", nullptr
    };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(
        0L, wc.lpszClassName, L"RmlUI Studio - Editor de interfaces", WS_OVERLAPPEDWINDOW,
        80, 60, 1440, 860, nullptr, nullptr, wc.hInstance, nullptr
    );
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    COLORREF cap_col = RGB(16, 19, 26);
    DwmSetWindowAttribute(hwnd, 35, &cap_col, sizeof(cap_col)); // DWMWA_CAPTION_COLOR

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        CoUninitialize();
        return 1;
    }

    Dx11Renderer2D dx2d;
    if (!dx2d.Init(g_pd3dDevice)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        CoUninitialize();
        return 1;
    }

    RmlEngineHost rml_engine;
    if (!rml_engine.Init(g_pd3dDevice, g_pd3dDeviceContext)) {
        dx2d.Shutdown();
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        CoUninitialize();
        return 1;
    }

    RmlDocument doc;

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    g_ClientWidth = (rc.right > rc.left) ? (int)(rc.right - rc.left) : 1440;
    g_ClientHeight = (rc.bottom > rc.top) ? (int)(rc.bottom - rc.top) : 860;

    StudioUI studio;
    g_studio = &studio;
    studio.Init(hwnd, doc, rml_engine);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        RECT cur_rc{};
        GetClientRect(hwnd, &cur_rc);
        int cur_w = (cur_rc.right > cur_rc.left) ? (int)(cur_rc.right - cur_rc.left) : g_ClientWidth;
        int cur_h = (cur_rc.bottom > cur_rc.top) ? (int)(cur_rc.bottom - cur_rc.top) : g_ClientHeight;
        if (cur_w != g_ClientWidth || cur_h != g_ClientHeight) {
            g_ClientWidth = cur_w;
            g_ClientHeight = cur_h;
            studio.OnResize(g_ClientWidth, g_ClientHeight);
        }

        const float clear_color[4] = {0.043f, 0.051f, 0.071f, 1.00f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);

        studio.UpdateAndRender(dx2d, g_pd3dDevice, g_pd3dDeviceContext, g_ClientWidth, g_ClientHeight);

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    g_studio = nullptr;
    studio.Shutdown();
    rml_engine.Shutdown();
    dx2d.Shutdown();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
