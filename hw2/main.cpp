#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cassert>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define SAFE_RELEASE(x) if (x) { x->Release(); x = nullptr; }

ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gContext = nullptr;
IDXGISwapChain* gSwapChain = nullptr;
ID3D11RenderTargetView* gRTV = nullptr;

ID3D11Buffer* gVertexBuffer = nullptr;
ID3D11Buffer* gIndexBuffer = nullptr;
ID3D11VertexShader* gVS = nullptr;
ID3D11PixelShader* gPS = nullptr;
ID3D11InputLayout* gInputLayout = nullptr;

void InitDirectX(HWND hWnd, int width, int height);
void Render();
void Resize(UINT width, UINT height);
void Cleanup();

// WINDOW PROC 

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

// WINMAIN 

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    const int WIDTH = 1280;
    const int HEIGHT = 720;

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"DX11Window";

    RegisterClass(&wc);

    RECT rc = { 0,0,WIDTH,HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hWnd = CreateWindow(
        wc.lpszClassName,
        L"Thu Hoai - NDC Triangle",
        WS_OVERLAPPEDWINDOW,
        100, 100,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hWnd, SW_SHOW);

    InitDirectX(hWnd, WIDTH, HEIGHT);

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

    // Device + Context
    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &gDevice,
        nullptr,
        &gContext);
    assert(SUCCEEDED(hr));

    // SwapChain
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGIDevice* dxgiDevice = nullptr;
    gDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);

    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);

    IDXGIFactory* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);

    factory->CreateSwapChain(gDevice, &scd, &gSwapChain);

    SAFE_RELEASE(factory);
    SAFE_RELEASE(adapter);
    SAFE_RELEASE(dxgiDevice);

    Resize(width, height);

    // GEOMETRY 

    struct Vertex
    {
        float x, y, z;
        float r, g, b, a;
    };

    Vertex vertices[] =
    {
        { -0.5f, -0.5f, 0.0f, 1,0,0,1 },
        {  0.5f, -0.5f, 0.0f, 0,1,0,1 },
        {  0.0f,  0.5f, 0.0f, 0,0,1,1 },
    };

    unsigned short indices[] = { 0, 2, 1 };

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(vertices);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = vertices;

    gDevice->CreateBuffer(&bd, &sd, &gVertexBuffer);

    bd.ByteWidth = sizeof(indices);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = indices;

    gDevice->CreateBuffer(&bd, &sd, &gIndexBuffer);

    // SHADERS 

    const char* vsSrc = R"(
        struct VSInput {
            float3 pos : POSITION;
            float4 color : COLOR;
        };
        struct VSOutput {
            float4 pos : SV_Position;
            float4 color : COLOR;
        };
        VSOutput vs(VSInput v) {
            VSOutput o;
            o.pos = float4(v.pos, 1.0);
            o.color = v.color;
            return o;
        }
    )";

    const char* psSrc = R"(
        struct VSOutput {
            float4 pos : SV_Position;
            float4 color : COLOR;
        };
        float4 ps(VSOutput p) : SV_Target {
            return p.color;
        }
    )";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;

    D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr,
        "vs", "vs_5_0", 0, 0, &vsBlob, nullptr);

    D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr,
        "ps", "ps_5_0", 0, 0, &psBlob, nullptr);

    gDevice->CreateVertexShader(
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        nullptr, &gVS);

    gDevice->CreatePixelShader(
        psBlob->GetBufferPointer(),
        psBlob->GetBufferSize(),
        nullptr, &gPS);

    D3D11_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    gDevice->CreateInputLayout(
        layout, 2,
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        &gInputLayout);

    SAFE_RELEASE(vsBlob);
    SAFE_RELEASE(psBlob);
}

//RESIZE

void Resize(UINT width, UINT height)
{
    SAFE_RELEASE(gRTV);

    gSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

    ID3D11Texture2D* backBuffer = nullptr;
    gSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);

    gDevice->CreateRenderTargetView(backBuffer, nullptr, &gRTV);
    SAFE_RELEASE(backBuffer);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    gContext->RSSetViewports(1, &vp);
}

void Render()
{
    gContext->OMSetRenderTargets(1, &gRTV, nullptr);

    float clearColor[4] = { 0.2f,0.3f,0.5f,1 };
    gContext->ClearRenderTargetView(gRTV, clearColor);

    UINT stride = sizeof(float) * 7;
    UINT offset = 0;

    gContext->IASetVertexBuffers(0, 1, &gVertexBuffer, &stride, &offset);
    gContext->IASetIndexBuffer(gIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
    gContext->IASetInputLayout(gInputLayout);
    gContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    gContext->VSSetShader(gVS, nullptr, 0);
    gContext->PSSetShader(gPS, nullptr, 0);

    gContext->DrawIndexed(3, 0, 0);

    gSwapChain->Present(1, 0);
}

void Cleanup()
{
    SAFE_RELEASE(gInputLayout);
    SAFE_RELEASE(gPS);
    SAFE_RELEASE(gVS);
    SAFE_RELEASE(gIndexBuffer);
    SAFE_RELEASE(gVertexBuffer);
    SAFE_RELEASE(gRTV);
    SAFE_RELEASE(gSwapChain);
    SAFE_RELEASE(gContext);
    SAFE_RELEASE(gDevice);
}
