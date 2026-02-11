#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cassert>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gContext = nullptr;
IDXGISwapChain* gSwapChain = nullptr;
ID3D11RenderTargetView* gRTV = nullptr;

#define SAFE_RELEASE(x) if (x) { x->Release(); x = nullptr; }

void InitDirectX(HWND hWnd, int width, int height);
void Render();
void Resize(UINT width, UINT height);
void Cleanup();

// Window procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        if (gSwapChain && wParam != SIZE_MINIMIZED)
        {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            Resize(width, height);
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}
 
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    const int WINDOW_WIDTH = 1280;
    const int WINDOW_HEIGHT = 720;

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"DX11WindowClass";

    RegisterClass(&wc);

    RECT rc = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hWnd = CreateWindow(
        wc.lpszClassName,
        L"DirectX 11 - Clear Color",
        WS_OVERLAPPEDWINDOW,
        100, 100,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    ShowWindow(hWnd, SW_SHOW);
    InitDirectX(hWnd, WINDOW_WIDTH, WINDOW_HEIGHT);

    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            Render();
        }
    }

    Cleanup();
    return 0;
}

void InitDirectX(HWND hWnd, int width, int height)
{
    HRESULT hr;
    IDXGIFactory* factory = nullptr;
    hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
    assert(SUCCEEDED(hr));

    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);

        if (wcscmp(desc.Description, L"Microsoft Basic Render Driver") != 0)
            break;

        SAFE_RELEASE(adapter);
    }
    assert(adapter);

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };

    hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        levels,
        1,
        D3D11_SDK_VERSION,
        &gDevice,
        &level,
        &gContext
    );
    assert(SUCCEEDED(hr));

    SAFE_RELEASE(adapter);

    // Create swap chain 
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = factory->CreateSwapChain(gDevice, &scd, &gSwapChain);
    assert(SUCCEEDED(hr));

    SAFE_RELEASE(factory);

    Resize(width, height);
}

// Resize swap chain and render target
void Resize(UINT width, UINT height)
{
    if (!gSwapChain) return;

    SAFE_RELEASE(gRTV);

    HRESULT hr = gSwapChain->ResizeBuffers(
        0,
        width,
        height,
        DXGI_FORMAT_UNKNOWN,
        0
    );
    assert(SUCCEEDED(hr));

    ID3D11Texture2D* backBuffer = nullptr;
    hr = gSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    assert(SUCCEEDED(hr));

    hr = gDevice->CreateRenderTargetView(backBuffer, nullptr, &gRTV);
    assert(SUCCEEDED(hr));

    SAFE_RELEASE(backBuffer);
}

void Render()
{
    gContext->OMSetRenderTargets(1, &gRTV, nullptr);

    const float clearColor[4] = { 0.2f, 0.3f, 0.5f, 1.0f };
    gContext->ClearRenderTargetView(gRTV, clearColor);

    HRESULT hr = gSwapChain->Present(1, 0);
    assert(SUCCEEDED(hr));
}

void Cleanup()
{
    SAFE_RELEASE(gRTV);
    SAFE_RELEASE(gSwapChain);
    SAFE_RELEASE(gContext);
    SAFE_RELEASE(gDevice);
}
