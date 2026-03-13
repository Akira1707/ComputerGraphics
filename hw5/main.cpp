#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wincodec.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <cfloat>
#include <cmath>
#include <algorithm>

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)  \
    ((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) |  \
    ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24))
#endif

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace DirectX;

// DDS
struct DDS_PIXELFORMAT
{
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwABitMask;
};

struct DDS_HEADER
{
    DWORD dwSize;
    DWORD dwHeaderFlags;
    DWORD dwHeight;
    DWORD dwWidth;
    DWORD dwPitchOrLinearSize;
    DWORD dwDepth;
    DWORD dwMipMapCount;
    DWORD dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    DWORD dwSurfaceFlags;
    DWORD dwCubemapFlags;
    DWORD dwReserved2[3];
};

#define DDS_MAGIC 0x20534444
#define DDS_SURFACE_FLAGS_MIPMAP 0x00400000
#define DDS_FOURCC 0x00000004
#define DDS_RGB 0x00000040
#define DDS_RGBA 0x00000041

#define FOURCC_DXT1 MAKEFOURCC('D','X','T','1')
#define FOURCC_DXT3 MAKEFOURCC('D','X','T','3')
#define FOURCC_DXT5 MAKEFOURCC('D','X','T','5')

// Helpers
UINT32 DivUp(UINT32 a, UINT32 b)
{
    return (a + b - 1) / b;
}

UINT32 GetBytesPerBlock(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC4_UNORM:
        return 8;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC5_UNORM:
        return 16;
    default:
        return 0;
    }
}

UINT32 BytesPerPixel(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return 4;
    default:
        return 0;
    }
}

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix)
{
    if (s.size() < suffix.size()) return false;
    return _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }

inline HRESULT SetResourceName(ID3D11DeviceChild* pResource, const std::string& name)
{
    if (!pResource) return E_POINTER;
    return pResource->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)name.length(), name.c_str());
}

// Texture loading
struct TextureDesc
{
    UINT32 pitch = 0;
    UINT32 mipmapsCount = 1;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT32 width = 0;
    UINT32 height = 0;
    void* pData = nullptr;

    std::vector<UINT32> mipPitches;
    std::vector<size_t> mipOffsets;
    size_t dataSize = 0;
};

bool LoadDDS(const wchar_t* filename, TextureDesc& desc)
{
    HANDLE hFile = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        OutputDebugStringA("Failed to open DDS file\n");
        return false;
    }

    DWORD dwMagic = 0;
    DWORD dwBytesRead = 0;
    if (!ReadFile(hFile, &dwMagic, sizeof(DWORD), &dwBytesRead, NULL) || dwBytesRead != sizeof(DWORD))
    {
        CloseHandle(hFile);
        return false;
    }

    if (dwMagic != DDS_MAGIC)
    {
        CloseHandle(hFile);
        OutputDebugStringA("Invalid DDS file\n");
        return false;
    }

    DDS_HEADER header = {};
    if (!ReadFile(hFile, &header, sizeof(DDS_HEADER), &dwBytesRead, NULL) || dwBytesRead != sizeof(DDS_HEADER))
    {
        CloseHandle(hFile);
        return false;
    }

    desc.width = header.dwWidth;
    desc.height = header.dwHeight;
    desc.mipmapsCount = (header.dwSurfaceFlags & DDS_SURFACE_FLAGS_MIPMAP) ? header.dwMipMapCount : 1;
    if (desc.mipmapsCount == 0)
        desc.mipmapsCount = 1;

    if (header.ddspf.dwFlags & DDS_FOURCC)
    {
        switch (header.ddspf.dwFourCC)
        {
        case FOURCC_DXT1: desc.fmt = DXGI_FORMAT_BC1_UNORM; break;
        case FOURCC_DXT3: desc.fmt = DXGI_FORMAT_BC2_UNORM; break;
        case FOURCC_DXT5: desc.fmt = DXGI_FORMAT_BC3_UNORM; break;
        default: desc.fmt = DXGI_FORMAT_UNKNOWN; break;
        }
    }
    else if ((header.ddspf.dwFlags & DDS_RGBA) &&
        header.ddspf.dwRGBBitCount == 32 &&
        header.ddspf.dwRBitMask == 0x00ff0000 &&
        header.ddspf.dwGBitMask == 0x0000ff00 &&
        header.ddspf.dwBBitMask == 0x000000ff &&
        header.ddspf.dwABitMask == 0xff000000)
    {
        desc.fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    else if ((header.ddspf.dwFlags & DDS_RGBA) &&
        header.ddspf.dwRGBBitCount == 32 &&
        header.ddspf.dwRBitMask == 0x000000ff &&
        header.ddspf.dwGBitMask == 0x0000ff00 &&
        header.ddspf.dwBBitMask == 0x00ff0000 &&
        header.ddspf.dwABitMask == 0xff000000)
    {
        desc.fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    else
    {
        desc.fmt = DXGI_FORMAT_UNKNOWN;
    }

    if (desc.fmt == DXGI_FORMAT_UNKNOWN)
    {
        CloseHandle(hFile);
        OutputDebugStringA("Unsupported DDS format\n");
        return false;
    }

    desc.mipPitches.clear();
    desc.mipOffsets.clear();
    desc.dataSize = 0;

    UINT32 mipWidth = desc.width;
    UINT32 mipHeight = desc.height;

    for (UINT32 mip = 0; mip < desc.mipmapsCount; ++mip)
    {
        UINT32 pitch = 0;
        size_t mipSize = 0;

        if (desc.fmt == DXGI_FORMAT_BC1_UNORM ||
            desc.fmt == DXGI_FORMAT_BC2_UNORM ||
            desc.fmt == DXGI_FORMAT_BC3_UNORM)
        {
            UINT32 blockWidth = DivUp(mipWidth, 4u);
            UINT32 blockHeight = DivUp(mipHeight, 4u);
            pitch = blockWidth * GetBytesPerBlock(desc.fmt);
            mipSize = (size_t)pitch * blockHeight;
        }
        else
        {
            pitch = mipWidth * BytesPerPixel(desc.fmt);
            mipSize = (size_t)pitch * mipHeight;
        }

        desc.mipOffsets.push_back(desc.dataSize);
        desc.mipPitches.push_back(pitch);
        desc.dataSize += mipSize;

        mipWidth = (mipWidth > 1) ? (mipWidth / 2) : 1;
        mipHeight = (mipHeight > 1) ? (mipHeight / 2) : 1;
    }

    desc.pitch = desc.mipPitches[0];

    desc.pData = malloc(desc.dataSize);
    if (!desc.pData)
    {
        CloseHandle(hFile);
        return false;
    }

    if (!ReadFile(hFile, desc.pData, (DWORD)desc.dataSize, &dwBytesRead, NULL) || dwBytesRead != desc.dataSize)
    {
        free(desc.pData);
        desc.pData = nullptr;
        CloseHandle(hFile);
        return false;
    }

    CloseHandle(hFile);
    return true;
}

bool LoadWICImage(const wchar_t* filename, TextureDesc& desc)
{
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    hr = factory->CreateDecoderFromFilename(
        filename, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr))
    {
        SAFE_RELEASE(factory);
        return false;
    }

    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        SAFE_RELEASE(decoder);
        SAFE_RELEASE(factory);
        return false;
    }

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr))
    {
        SAFE_RELEASE(frame);
        SAFE_RELEASE(decoder);
        SAFE_RELEASE(factory);
        return false;
    }

    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr))
    {
        SAFE_RELEASE(frame);
        SAFE_RELEASE(decoder);
        SAFE_RELEASE(factory);
        return false;
    }

    hr = converter->Initialize(
        frame,
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        SAFE_RELEASE(converter);
        SAFE_RELEASE(frame);
        SAFE_RELEASE(decoder);
        SAFE_RELEASE(factory);
        return false;
    }

    desc.width = w;
    desc.height = h;
    desc.mipmapsCount = 1;
    desc.fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.pitch = w * 4;
    desc.dataSize = (size_t)desc.pitch * desc.height;

    desc.pData = malloc(desc.dataSize);
    if (!desc.pData)
    {
        SAFE_RELEASE(converter);
        SAFE_RELEASE(frame);
        SAFE_RELEASE(decoder);
        SAFE_RELEASE(factory);
        return false;
    }

    hr = converter->CopyPixels(nullptr, desc.pitch, (UINT)desc.dataSize, reinterpret_cast<BYTE*>(desc.pData));
    if (FAILED(hr))
    {
        free(desc.pData);
        desc.pData = nullptr;
        SAFE_RELEASE(converter);
        SAFE_RELEASE(frame);
        SAFE_RELEASE(decoder);
        SAFE_RELEASE(factory);
        return false;
    }

    SAFE_RELEASE(converter);
    SAFE_RELEASE(frame);
    SAFE_RELEASE(decoder);
    SAFE_RELEASE(factory);
    return true;
}

bool LoadImageAny(const wchar_t* filename, TextureDesc& desc)
{
    std::wstring name(filename);
    if (EndsWithNoCase(name, L".dds"))
        return LoadDDS(filename, desc);

    if (EndsWithNoCase(name, L".png") ||
        EndsWithNoCase(name, L".jpg") ||
        EndsWithNoCase(name, L".jpeg") ||
        EndsWithNoCase(name, L".bmp"))
        return LoadWICImage(filename, desc);

    return false;
}


// Globals
HWND g_hWnd = nullptr;

ID3D11Device* g_pDevice = nullptr;
ID3D11DeviceContext* g_pDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_pBackBufferRTV = nullptr;
ID3D11DepthStencilView* g_pDepthStencilView = nullptr;

struct TextureVertex
{
    float x, y, z;
    float u, v;
};

ID3D11Buffer* g_pVertexBuffer = nullptr;
ID3D11Buffer* g_pIndexBuffer = nullptr;

ID3D11VertexShader* g_pVertexShader = nullptr;
ID3D11PixelShader* g_pPixelShader = nullptr;
ID3D11InputLayout* g_pInputLayout = nullptr;

ID3D11VertexShader* g_pSkyboxVS = nullptr;
ID3D11PixelShader* g_pSkyboxPS = nullptr;
ID3D11InputLayout* g_pSkyboxInputLayout = nullptr;
ID3D11Buffer* g_pSkyboxVertexBuffer = nullptr;
ID3D11Buffer* g_pSkyboxIndexBuffer = nullptr;

ID3D11VertexShader* g_pTransparentVS = nullptr;
ID3D11PixelShader* g_pTransparentPS = nullptr;
ID3D11InputLayout* g_pTransparentInputLayout = nullptr;

struct ModelBuffer
{
    XMMATRIX model;
};

struct ViewProjBuffer
{
    XMMATRIX vp;
};

struct TransparentBuffer
{
    XMMATRIX model;
    XMFLOAT4 color;
};

ID3D11Buffer* g_pModelBuffer = nullptr;
ID3D11Buffer* g_pViewProjBuffer = nullptr;
ID3D11Buffer* g_pTransparentBuffer = nullptr;

ID3D11Texture2D* g_pTexture = nullptr;
ID3D11ShaderResourceView* g_pTextureView = nullptr;
ID3D11SamplerState* g_pSampler = nullptr;

ID3D11Texture2D* g_pCubemapTexture = nullptr;
ID3D11ShaderResourceView* g_pCubemapView = nullptr;

ID3D11BlendState* g_pTransparentBlendState = nullptr;
ID3D11DepthStencilState* g_pTransparentDepthState = nullptr;
ID3D11DepthStencilState* g_pOpaqueDepthState = nullptr;
ID3D11DepthStencilState* g_pSkyboxDepthState = nullptr;
ID3D11RasterizerState* g_pCullBackRS = nullptr;
ID3D11RasterizerState* g_pCullNoneRS = nullptr;

UINT g_ClientWidth = 1280;
UINT g_ClientHeight = 720;

float g_CameraYaw = 0.0f;
float g_CameraPitch = 0.3f;
float g_CameraDist = 3.0f;
bool g_KeyLeft = false, g_KeyRight = false, g_KeyUp = false, g_KeyDown = false;

double g_LastTime = 0.0;

// Transparent objects
struct TransparentObject
{
    XMMATRIX model;
    XMFLOAT4 color;
    XMFLOAT3 center;
    float distanceToCamera;
};

// Prototypes
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
bool InitDirectX();
void CreateCubeResources();
void CompileShaders();
void LoadTextures();
bool CreateRenderStates();
void CleanupDirectX();
void RenderFrame();
void OnResize(UINT newWidth, UINT newHeight);
void UpdateCamera(double deltaTime);

// WinMain
int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX11";

    if (!RegisterClassExW(&wc))
    {
        MessageBoxW(nullptr, L"RegisterClassEx failed", L"Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 0;
    }

    RECT rc = { 0, 0, (LONG)g_ClientWidth, (LONG)g_ClientHeight };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    int winWidth = rc.right - rc.left;
    int winHeight = rc.bottom - rc.top;

    g_hWnd = CreateWindowW(wc.lpszClassName, L"Thu Hoai - Depth + Transparency + Skybox",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        winWidth, winHeight, nullptr, nullptr, hInstance, nullptr);
    if (!g_hWnd)
    {
        MessageBoxW(nullptr, L"CreateWindow failed", L"Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 0;
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    if (!InitDirectX())
    {
        CleanupDirectX();
        DestroyWindow(g_hWnd);
        CoUninitialize();
        return -1;
    }

    CreateCubeResources();
    CompileShaders();
    LoadTextures();
    if (!CreateRenderStates())
    {
        CleanupDirectX();
        DestroyWindow(g_hWnd);
        CoUninitialize();
        return -1;
    }

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(ModelBuffer);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HRESULT hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pModelBuffer);
    assert(SUCCEEDED(hr));
    SetResourceName(g_pModelBuffer, "ModelBuffer");

    desc = {};
    desc.ByteWidth = sizeof(ViewProjBuffer);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pViewProjBuffer);
    assert(SUCCEEDED(hr));
    SetResourceName(g_pViewProjBuffer, "ViewProjBuffer");

    desc = {};
    desc.ByteWidth = sizeof(TransparentBuffer);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pTransparentBuffer);
    assert(SUCCEEDED(hr));
    SetResourceName(g_pTransparentBuffer, "TransparentBuffer");

    g_LastTime = (double)GetTickCount64() / 1000.0;

    MSG msg = {};
    bool done = false;
    while (!done)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                done = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!done)
            RenderFrame();
    }

    CleanupDirectX();
    CoUninitialize();
    return (int)msg.wParam;
}

// Window proc
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_SIZE:
        if (g_pSwapChain && wParam != SIZE_MINIMIZED)
        {
            UINT newW = LOWORD(lParam);
            UINT newH = HIWORD(lParam);
            if (newW > 0 && newH > 0)
                OnResize(newW, newH);
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_LEFT)  g_KeyLeft = true;
        if (wParam == VK_RIGHT) g_KeyRight = true;
        if (wParam == VK_UP)    g_KeyUp = true;
        if (wParam == VK_DOWN)  g_KeyDown = true;
        return 0;

    case WM_KEYUP:
        if (wParam == VK_LEFT)  g_KeyLeft = false;
        if (wParam == VK_RIGHT) g_KeyRight = false;
        if (wParam == VK_UP)    g_KeyUp = false;
        if (wParam == VK_DOWN)  g_KeyDown = false;
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// DirectX init
bool InitDirectX()
{
    HRESULT hr;

    IDXGIFactory* pFactory = nullptr;
    hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory);
    if (FAILED(hr)) return false;

    IDXGIAdapter* pSelectedAdapter = nullptr;
    UINT idx = 0;
    while (true)
    {
        IDXGIAdapter* pAdapter = nullptr;
        hr = pFactory->EnumAdapters(idx, &pAdapter);
        if (FAILED(hr) || pAdapter == nullptr) break;

        DXGI_ADAPTER_DESC desc;
        pAdapter->GetDesc(&desc);
        if (wcscmp(desc.Description, L"Microsoft Basic Render Driver") != 0)
        {
            pSelectedAdapter = pAdapter;
            break;
        }
        pAdapter->Release();
        idx++;
    }

    if (!pSelectedAdapter)
    {
        pFactory->Release();
        return false;
    }

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL obtainedLevel;

    hr = D3D11CreateDevice(
        pSelectedAdapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        levels,
        1,
        D3D11_SDK_VERSION,
        &g_pDevice,
        &obtainedLevel,
        &g_pDeviceContext);
    pSelectedAdapter->Release();

    if (FAILED(hr) || obtainedLevel != D3D_FEATURE_LEVEL_11_0)
    {
        pFactory->Release();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = g_ClientWidth;
    scd.BufferDesc.Height = g_ClientHeight;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 0;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = g_hWnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = 0;

    hr = pFactory->CreateSwapChain(g_pDevice, &scd, &g_pSwapChain);
    pFactory->Release();
    if (FAILED(hr)) return false;

    ID3D11Texture2D* pBackBuffer = nullptr;
    hr = g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = g_ClientWidth;
    depthDesc.Height = g_ClientHeight;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ID3D11Texture2D* pDepthStencil = nullptr;
    hr = g_pDevice->CreateTexture2D(&depthDesc, nullptr, &pDepthStencil);
    if (FAILED(hr))
    {
        pBackBuffer->Release();
        return false;
    }

    hr = g_pDevice->CreateDepthStencilView(pDepthStencil, nullptr, &g_pDepthStencilView);
    pDepthStencil->Release();
    if (FAILED(hr))
    {
        pBackBuffer->Release();
        return false;
    }

    hr = g_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pBackBufferRTV);
    pBackBuffer->Release();
    if (FAILED(hr)) return false;

    return true;
}

// Geometry
void CreateCubeResources()
{
    const TextureVertex cubeVertices[] = {
        {-0.5f, -0.5f, -0.5f, 0.0f, 1.0f},
        { 0.5f, -0.5f, -0.5f, 1.0f, 1.0f},
        { 0.5f,  0.5f, -0.5f, 1.0f, 0.0f},
        {-0.5f,  0.5f, -0.5f, 0.0f, 0.0f},

        {-0.5f, -0.5f,  0.5f, 0.0f, 1.0f},
        { 0.5f, -0.5f,  0.5f, 1.0f, 1.0f},
        { 0.5f,  0.5f,  0.5f, 1.0f, 0.0f},
        {-0.5f,  0.5f,  0.5f, 0.0f, 0.0f},

        {-0.5f, -0.5f,  0.5f, 0.0f, 1.0f},
        {-0.5f, -0.5f, -0.5f, 1.0f, 1.0f},
        {-0.5f,  0.5f, -0.5f, 1.0f, 0.0f},
        {-0.5f,  0.5f,  0.5f, 0.0f, 0.0f},

        { 0.5f, -0.5f, -0.5f, 0.0f, 1.0f},
        { 0.5f, -0.5f,  0.5f, 1.0f, 1.0f},
        { 0.5f,  0.5f,  0.5f, 1.0f, 0.0f},
        { 0.5f,  0.5f, -0.5f, 0.0f, 0.0f},

        {-0.5f,  0.5f, -0.5f, 0.0f, 1.0f},
        { 0.5f,  0.5f, -0.5f, 1.0f, 1.0f},
        { 0.5f,  0.5f,  0.5f, 1.0f, 0.0f},
        {-0.5f,  0.5f,  0.5f, 0.0f, 0.0f},

        {-0.5f, -0.5f,  0.5f, 0.0f, 1.0f},
        { 0.5f, -0.5f,  0.5f, 1.0f, 1.0f},
        { 0.5f, -0.5f, -0.5f, 1.0f, 0.0f},
        {-0.5f, -0.5f, -0.5f, 0.0f, 0.0f}
    };

    const USHORT cubeIndices[] = {
        0, 2, 1,  0, 3, 2,
        4, 5, 6,  4, 6, 7,
        8, 10, 9,  8, 11, 10,
        12, 14, 13,  12, 15, 14,
        16, 18, 17,  16, 19, 18,
        20, 22, 21,  20, 23, 22
    };

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(cubeVertices);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data = { cubeVertices };
    HRESULT hr = g_pDevice->CreateBuffer(&desc, &data, &g_pVertexBuffer);
    if (FAILED(hr)) return;
    SetResourceName(g_pVertexBuffer, "CubeVertexBuffer");

    desc = {};
    desc.ByteWidth = sizeof(cubeIndices);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = cubeIndices;
    hr = g_pDevice->CreateBuffer(&desc, &data, &g_pIndexBuffer);
    if (FAILED(hr)) return;
    SetResourceName(g_pIndexBuffer, "CubeIndexBuffer");

    const TextureVertex skyboxVertices[] = {
        {-10, -10, -10, 0, 0}, { 10, -10, -10, 0, 0}, { 10,  10, -10, 0, 0}, {-10,  10, -10, 0, 0},
        {-10, -10,  10, 0, 0}, { 10, -10,  10, 0, 0}, { 10,  10,  10, 0, 0}, {-10,  10,  10, 0, 0}
    };

    const USHORT skyboxIndices[] = {
        0,2,1, 0,3,2,
        4,5,6, 4,6,7,
        0,7,3, 0,4,7,
        1,2,6, 1,6,5,
        3,7,6, 3,6,2,
        0,1,5, 0,5,4
    };

    desc = {};
    desc.ByteWidth = sizeof(skyboxVertices);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    data.pSysMem = skyboxVertices;
    hr = g_pDevice->CreateBuffer(&desc, &data, &g_pSkyboxVertexBuffer);
    if (FAILED(hr)) return;
    SetResourceName(g_pSkyboxVertexBuffer, "SkyboxVertexBuffer");

    desc = {};
    desc.ByteWidth = sizeof(skyboxIndices);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = skyboxIndices;
    hr = g_pDevice->CreateBuffer(&desc, &data, &g_pSkyboxIndexBuffer);
    if (FAILED(hr)) return;
    SetResourceName(g_pSkyboxIndexBuffer, "SkyboxIndexBuffer");
}

// Shaders
void CompileShaders()
{
    const char* cubeVS = R"(
        cbuffer ModelBuffer : register(b0)
        {
            float4x4 model;
        }
        cbuffer ViewProjBuffer : register(b1)
        {
            float4x4 vp;
        }
        struct VSInput
        {
            float3 pos : POSITION;
            float2 uv : TEXCOORD;
        };
        struct VSOutput
        {
            float4 pos : SV_Position;
            float2 uv : TEXCOORD;
        };
        VSOutput vs(VSInput vertex)
        {
            VSOutput result;
            float4 worldPos = mul(float4(vertex.pos, 1.0), model);
            result.pos = mul(worldPos, vp);
            result.uv = vertex.uv;
            return result;
        }
    )";

    const char* cubePS = R"(
        Texture2D colorTexture : register(t0);
        SamplerState colorSampler : register(s0);
        struct VSOutput
        {
            float4 pos : SV_Position;
            float2 uv : TEXCOORD;
        };
        float4 ps(VSOutput pixel) : SV_Target0
        {
            return colorTexture.Sample(colorSampler, pixel.uv);
        }
    )";

    const char* skyboxVS = R"(
        cbuffer ViewProjBuffer : register(b1)
        {
            float4x4 vp;
        }
        struct VSInput
        {
            float3 pos : POSITION;
            float2 uv : TEXCOORD;
        };
        struct VSOutput
        {
            float4 pos : SV_Position;
            float3 localPos : TEXCOORD;
        };
        VSOutput vs(VSInput vertex)
        {
            VSOutput result;
            result.pos = mul(float4(vertex.pos, 1.0), vp);
            result.localPos = vertex.pos;
            return result;
        }
    )";

    const char* skyboxPS = R"(
        TextureCube skyboxTexture : register(t1);
        SamplerState skyboxSampler : register(s1);
        struct VSOutput
        {
            float4 pos : SV_Position;
            float3 localPos : TEXCOORD;
        };
        float4 ps(VSOutput pixel) : SV_Target0
        {
            return skyboxTexture.Sample(skyboxSampler, pixel.localPos);
        }
    )";

    const char* transparentVS = R"(
        cbuffer TransparentBuffer : register(b0)
        {
            float4x4 model;
            float4 color;
        }
        cbuffer ViewProjBuffer : register(b1)
        {
            float4x4 vp;
        }
        struct VSInput
        {
            float3 pos : POSITION;
            float2 uv : TEXCOORD;
        };
        struct VSOutput
        {
            float4 pos : SV_Position;
            float4 color : COLOR0;
        };
        VSOutput vs(VSInput vertex)
        {
            VSOutput result;
            float4 worldPos = mul(float4(vertex.pos, 1.0), model);
            result.pos = mul(worldPos, vp);
            result.color = color;
            return result;
        }
    )";

    const char* transparentPS = R"(
        struct VSOutput
        {
            float4 pos : SV_Position;
            float4 color : COLOR0;
        };
        float4 ps(VSOutput pixel) : SV_Target0
        {
            return pixel.color;
        }
    )";

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ID3DBlob* pVsBlob = nullptr;
    ID3DBlob* pPsBlob = nullptr;
    ID3DBlob* pErrorBlob = nullptr;
    HRESULT hr;

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };

    hr = D3DCompile(cubeVS, strlen(cubeVS), nullptr, nullptr, nullptr, "vs", "vs_5_0", flags, 0, &pVsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        if (pErrorBlob) OutputDebugStringA((const char*)pErrorBlob->GetBufferPointer());
        SAFE_RELEASE(pErrorBlob);
        return;
    }
    hr = g_pDevice->CreateVertexShader(pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), nullptr, &g_pVertexShader);
    if (FAILED(hr)) return;
    hr = g_pDevice->CreateInputLayout(layoutDesc, 2, pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), &g_pInputLayout);
    if (FAILED(hr)) return;
    SAFE_RELEASE(pVsBlob);

    hr = D3DCompile(cubePS, strlen(cubePS), nullptr, nullptr, nullptr, "ps", "ps_5_0", flags, 0, &pPsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        if (pErrorBlob) OutputDebugStringA((const char*)pErrorBlob->GetBufferPointer());
        SAFE_RELEASE(pErrorBlob);
        return;
    }
    hr = g_pDevice->CreatePixelShader(pPsBlob->GetBufferPointer(), pPsBlob->GetBufferSize(), nullptr, &g_pPixelShader);
    if (FAILED(hr)) return;
    SAFE_RELEASE(pPsBlob);

    hr = D3DCompile(skyboxVS, strlen(skyboxVS), nullptr, nullptr, nullptr, "vs", "vs_5_0", flags, 0, &pVsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        if (pErrorBlob) OutputDebugStringA((const char*)pErrorBlob->GetBufferPointer());
        SAFE_RELEASE(pErrorBlob);
        return;
    }
    hr = g_pDevice->CreateVertexShader(pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), nullptr, &g_pSkyboxVS);
    if (FAILED(hr)) return;
    hr = g_pDevice->CreateInputLayout(layoutDesc, 2, pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), &g_pSkyboxInputLayout);
    if (FAILED(hr)) return;
    SAFE_RELEASE(pVsBlob);

    hr = D3DCompile(skyboxPS, strlen(skyboxPS), nullptr, nullptr, nullptr, "ps", "ps_5_0", flags, 0, &pPsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        if (pErrorBlob) OutputDebugStringA((const char*)pErrorBlob->GetBufferPointer());
        SAFE_RELEASE(pErrorBlob);
        return;
    }
    hr = g_pDevice->CreatePixelShader(pPsBlob->GetBufferPointer(), pPsBlob->GetBufferSize(), nullptr, &g_pSkyboxPS);
    if (FAILED(hr)) return;
    SAFE_RELEASE(pPsBlob);

    hr = D3DCompile(transparentVS, strlen(transparentVS), nullptr, nullptr, nullptr, "vs", "vs_5_0", flags, 0, &pVsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        if (pErrorBlob) OutputDebugStringA((const char*)pErrorBlob->GetBufferPointer());
        SAFE_RELEASE(pErrorBlob);
        return;
    }
    hr = g_pDevice->CreateVertexShader(pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), nullptr, &g_pTransparentVS);
    if (FAILED(hr)) return;
    hr = g_pDevice->CreateInputLayout(layoutDesc, 2, pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), &g_pTransparentInputLayout);
    if (FAILED(hr)) return;
    SAFE_RELEASE(pVsBlob);

    hr = D3DCompile(transparentPS, strlen(transparentPS), nullptr, nullptr, nullptr, "ps", "ps_5_0", flags, 0, &pPsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        if (pErrorBlob) OutputDebugStringA((const char*)pErrorBlob->GetBufferPointer());
        SAFE_RELEASE(pErrorBlob);
        return;
    }
    hr = g_pDevice->CreatePixelShader(pPsBlob->GetBufferPointer(), pPsBlob->GetBufferSize(), nullptr, &g_pTransparentPS);
    if (FAILED(hr)) return;
    SAFE_RELEASE(pPsBlob);

    SAFE_RELEASE(pErrorBlob);
}

// Textures
void LoadTextures()
{
    HRESULT hr = S_OK;

    const std::wstring cubeTexName = L"cube.dds";
    TextureDesc texDesc;
    if (!LoadImageAny(cubeTexName.c_str(), texDesc))
    {
        MessageBoxA(NULL, "Failed to load cube.dds", "Error", MB_OK);
        return;
    }

    D3D11_TEXTURE2D_DESC tex2DDesc = {};
    tex2DDesc.Width = texDesc.width;
    tex2DDesc.Height = texDesc.height;
    tex2DDesc.MipLevels = texDesc.mipmapsCount;
    tex2DDesc.ArraySize = 1;
    tex2DDesc.Format = texDesc.fmt;
    tex2DDesc.SampleDesc.Count = 1;
    tex2DDesc.SampleDesc.Quality = 0;
    tex2DDesc.Usage = D3D11_USAGE_IMMUTABLE;
    tex2DDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    tex2DDesc.CPUAccessFlags = 0;
    tex2DDesc.MiscFlags = 0;

    std::vector<D3D11_SUBRESOURCE_DATA> texData(texDesc.mipmapsCount);
    for (UINT32 mip = 0; mip < texDesc.mipmapsCount; ++mip)
    {
        texData[mip].pSysMem = reinterpret_cast<const BYTE*>(texDesc.pData) + texDesc.mipOffsets[mip];
        texData[mip].SysMemPitch = texDesc.mipPitches[mip];
        texData[mip].SysMemSlicePitch = 0;
    }

    hr = g_pDevice->CreateTexture2D(&tex2DDesc, texData.data(), &g_pTexture);
    if (FAILED(hr) || !g_pTexture)
    {
        char err[128];
        sprintf_s(err, "CreateTexture2D failed: 0x%08X", hr);
        MessageBoxA(NULL, err, "Error", MB_OK);
        free(texDesc.pData);
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.fmt;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = texDesc.mipmapsCount;
    srvDesc.Texture2D.MostDetailedMip = 0;

    hr = g_pDevice->CreateShaderResourceView(g_pTexture, &srvDesc, &g_pTextureView);
    if (FAILED(hr) || !g_pTextureView)
    {
        char err[128];
        sprintf_s(err, "CreateSRV failed: 0x%08X", hr);
        MessageBoxA(NULL, err, "Error", MB_OK);
        free(texDesc.pData);
        return;
    }

    free(texDesc.pData);

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.MinLOD = -FLT_MAX;
    sampDesc.MaxLOD = FLT_MAX;
    sampDesc.MipLODBias = 0.0f;
    sampDesc.MaxAnisotropy = 16;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.BorderColor[0] = 1.0f;
    sampDesc.BorderColor[1] = 1.0f;
    sampDesc.BorderColor[2] = 1.0f;
    sampDesc.BorderColor[3] = 1.0f;

    hr = g_pDevice->CreateSamplerState(&sampDesc, &g_pSampler);
    if (FAILED(hr) || !g_pSampler)
    {
        MessageBoxA(NULL, "CreateSampler failed", "Error", MB_OK);
        return;
    }

    const std::wstring faceNames[6] = {
        L"Skybox/posx.png",
        L"Skybox/negx.png",
        L"Skybox/posy.png",
        L"Skybox/negy.png",
        L"Skybox/posz.png",
        L"Skybox/negz.png"
    };

    TextureDesc faceDescs[6];
    bool allOk = true;
    for (int i = 0; i < 6; ++i)
    {
        if (!LoadImageAny(faceNames[i].c_str(), faceDescs[i]))
        {
            allOk = false;
            break;
        }
    }

    if (!allOk)
    {
        MessageBoxA(NULL, "Failed to load cubemap faces", "Error", MB_OK);
        for (int i = 0; i < 6; ++i)
            if (faceDescs[i].pData) free(faceDescs[i].pData);
        return;
    }

    for (int i = 1; i < 6; ++i)
    {
        if (faceDescs[i].fmt != faceDescs[0].fmt ||
            faceDescs[i].width != faceDescs[0].width ||
            faceDescs[i].height != faceDescs[0].height)
        {
            MessageBoxA(NULL, "Cubemap faces must have same format and size", "Error", MB_OK);
            for (int j = 0; j < 6; ++j)
                if (faceDescs[j].pData) free(faceDescs[j].pData);
            return;
        }
    }

    D3D11_TEXTURE2D_DESC cubeDesc = {};
    cubeDesc.Width = faceDescs[0].width;
    cubeDesc.Height = faceDescs[0].height;
    cubeDesc.MipLevels = 1;
    cubeDesc.ArraySize = 6;
    cubeDesc.Format = faceDescs[0].fmt;
    cubeDesc.SampleDesc.Count = 1;
    cubeDesc.SampleDesc.Quality = 0;
    cubeDesc.Usage = D3D11_USAGE_IMMUTABLE;
    cubeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cubeDesc.CPUAccessFlags = 0;
    cubeDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    D3D11_SUBRESOURCE_DATA initData[6] = {};
    for (int i = 0; i < 6; ++i)
    {
        initData[i].pSysMem = faceDescs[i].pData;
        initData[i].SysMemPitch = faceDescs[i].pitch;
        initData[i].SysMemSlicePitch = 0;
    }

    hr = g_pDevice->CreateTexture2D(&cubeDesc, initData, &g_pCubemapTexture);

    for (int i = 0; i < 6; ++i)
        free(faceDescs[i].pData);

    if (FAILED(hr) || !g_pCubemapTexture)
    {
        char err[128];
        sprintf_s(err, "CreateCubemapTexture failed: 0x%08X", hr);
        MessageBoxA(NULL, err, "Error", MB_OK);
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC cubeSRVDesc = {};
    cubeSRVDesc.Format = cubeDesc.Format;
    cubeSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    cubeSRVDesc.TextureCube.MipLevels = 1;
    cubeSRVDesc.TextureCube.MostDetailedMip = 0;

    hr = g_pDevice->CreateShaderResourceView(g_pCubemapTexture, &cubeSRVDesc, &g_pCubemapView);
    if (FAILED(hr) || !g_pCubemapView)
    {
        MessageBoxA(NULL, "CreateCubemapSRV failed", "Error", MB_OK);
        return;
    }
}

// States
bool CreateRenderStates()
{
    HRESULT hr;

    D3D11_DEPTH_STENCIL_DESC opaqueDesc = {};
    opaqueDesc.DepthEnable = TRUE;
    opaqueDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    opaqueDesc.DepthFunc = D3D11_COMPARISON_LESS;
    opaqueDesc.StencilEnable = FALSE;
    hr = g_pDevice->CreateDepthStencilState(&opaqueDesc, &g_pOpaqueDepthState);
    if (FAILED(hr)) return false;

    D3D11_DEPTH_STENCIL_DESC skyboxDesc = {};
    skyboxDesc.DepthEnable = TRUE;
    skyboxDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    skyboxDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    skyboxDesc.StencilEnable = FALSE;
    hr = g_pDevice->CreateDepthStencilState(&skyboxDesc, &g_pSkyboxDepthState);
    if (FAILED(hr)) return false;

    D3D11_DEPTH_STENCIL_DESC transparentDesc = {};
    transparentDesc.DepthEnable = TRUE;
    transparentDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    transparentDesc.DepthFunc = D3D11_COMPARISON_LESS;
    transparentDesc.StencilEnable = FALSE;
    hr = g_pDevice->CreateDepthStencilState(&transparentDesc, &g_pTransparentDepthState);
    if (FAILED(hr)) return false;

    D3D11_RASTERIZER_DESC rsBack = {};
    rsBack.FillMode = D3D11_FILL_SOLID;
    rsBack.CullMode = D3D11_CULL_BACK;
    rsBack.FrontCounterClockwise = FALSE;
    rsBack.DepthClipEnable = TRUE;
    hr = g_pDevice->CreateRasterizerState(&rsBack, &g_pCullBackRS);
    if (FAILED(hr)) return false;

    D3D11_RASTERIZER_DESC rsNone = {};
    rsNone.FillMode = D3D11_FILL_SOLID;
    rsNone.CullMode = D3D11_CULL_NONE;
    rsNone.FrontCounterClockwise = FALSE;
    rsNone.DepthClipEnable = TRUE;
    hr = g_pDevice->CreateRasterizerState(&rsNone, &g_pCullNoneRS);
    if (FAILED(hr)) return false;

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_pDevice->CreateBlendState(&blendDesc, &g_pTransparentBlendState);
    if (FAILED(hr)) return false;

    return true;
}

// Camera
void UpdateCamera(double deltaTime)
{
    float speed = 1.0f;
    if (g_KeyLeft)  g_CameraYaw -= speed * (float)deltaTime;
    if (g_KeyRight) g_CameraYaw += speed * (float)deltaTime;
    if (g_KeyUp)    g_CameraPitch += speed * (float)deltaTime;
    if (g_KeyDown)  g_CameraPitch -= speed * (float)deltaTime;

    if (g_CameraPitch > 1.5f) g_CameraPitch = 1.5f;
    if (g_CameraPitch < -1.5f) g_CameraPitch = -1.5f;
}

// Render
void RenderFrame()
{
    if (!g_pDeviceContext || !g_pBackBufferRTV || !g_pSwapChain)
        return;

    double currentTime = (double)GetTickCount64() / 1000.0;
    double deltaTime = currentTime - g_LastTime;
    g_LastTime = currentTime;

    UpdateCamera(deltaTime);

    g_pDeviceContext->ClearState();

    ID3D11RenderTargetView* views[] = { g_pBackBufferRTV };
    g_pDeviceContext->OMSetRenderTargets(1, views, g_pDepthStencilView);

    const FLOAT clearColor[4] = { 0.25f, 0.25f, 0.25f, 1.0f };
    g_pDeviceContext->ClearRenderTargetView(g_pBackBufferRTV, clearColor);
    g_pDeviceContext->ClearDepthStencilView(g_pDepthStencilView, D3D11_CLEAR_DEPTH, 1.0f, 0);

    D3D11_VIEWPORT viewport = { 0, 0, (FLOAT)g_ClientWidth, (FLOAT)g_ClientHeight, 0.0f, 1.0f };
    g_pDeviceContext->RSSetViewports(1, &viewport);

    float angle = (float)currentTime * 0.5f;

    float camX = g_CameraDist * sinf(g_CameraYaw) * cosf(g_CameraPitch);
    float camY = g_CameraDist * sinf(g_CameraPitch);
    float camZ = g_CameraDist * cosf(g_CameraYaw) * cosf(g_CameraPitch);

    XMVECTOR eye = XMVectorSet(camX, camY, camZ, 0);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    XMMATRIX view = XMMatrixLookAtLH(eye, target, up);
    float aspect = (float)g_ClientWidth / (float)g_ClientHeight;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PI / 3.0f, aspect, 0.1f, 100.0f);
    XMMATRIX vp = XMMatrixMultiply(view, proj);

    ID3D11SamplerState* samplers[] = { g_pSampler };
    g_pDeviceContext->PSSetSamplers(0, 1, samplers);
    g_pDeviceContext->PSSetSamplers(1, 1, samplers);

    UINT stride = sizeof(TextureVertex);
    UINT offset = 0;

    // OPAQUE OBJECTS
    g_pDeviceContext->OMSetDepthStencilState(g_pOpaqueDepthState, 0);
    g_pDeviceContext->RSSetState(g_pCullBackRS);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = g_pDeviceContext->Map(g_pViewProjBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        ViewProjBuffer* pVP = (ViewProjBuffer*)mapped.pData;
        XMStoreFloat4x4((XMFLOAT4X4*)&pVP->vp, XMMatrixTranspose(vp));
        g_pDeviceContext->Unmap(g_pViewProjBuffer, 0);
    }

    g_pDeviceContext->VSSetShader(g_pVertexShader, nullptr, 0);
    g_pDeviceContext->PSSetShader(g_pPixelShader, nullptr, 0);
    g_pDeviceContext->IASetInputLayout(g_pInputLayout);

    ID3D11Buffer* vbCube[] = { g_pVertexBuffer };
    g_pDeviceContext->IASetVertexBuffers(0, 1, vbCube, &stride, &offset);
    g_pDeviceContext->IASetIndexBuffer(g_pIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
    g_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11ShaderResourceView* cubeSRV[] = { g_pTextureView };
    g_pDeviceContext->PSSetShaderResources(0, 1, cubeSRV);

    ID3D11Buffer* cbsCube[] = { g_pModelBuffer, g_pViewProjBuffer };
    g_pDeviceContext->VSSetConstantBuffers(0, 2, cbsCube);

    // cube 1
    XMMATRIX model1 = XMMatrixRotationY(angle);
    ModelBuffer modelData1;
    XMStoreFloat4x4((XMFLOAT4X4*)&modelData1.model, XMMatrixTranspose(model1));
    g_pDeviceContext->UpdateSubresource(g_pModelBuffer, 0, nullptr, &modelData1, 0, 0);
    g_pDeviceContext->DrawIndexed(36, 0, 0);

    // cube 2
    XMMATRIX model2 =
        XMMatrixScaling(0.7f, 0.7f, 0.7f) *
        XMMatrixRotationX(angle * 0.5f) *
        XMMatrixTranslation(1.5f, 0.0f, 0.7f);

    ModelBuffer modelData2;
    XMStoreFloat4x4((XMFLOAT4X4*)&modelData2.model, XMMatrixTranspose(model2));
    g_pDeviceContext->UpdateSubresource(g_pModelBuffer, 0, nullptr, &modelData2, 0, 0);
    g_pDeviceContext->DrawIndexed(36, 0, 0);

    // SKYBOX

    XMMATRIX viewNoTranslate = view;
    viewNoTranslate.r[3] = XMVectorSet(0, 0, 0, 1);
    XMMATRIX vpSky = XMMatrixMultiply(viewNoTranslate, proj);

    D3D11_MAPPED_SUBRESOURCE mappedSky = {};
    hr = g_pDeviceContext->Map(g_pViewProjBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedSky);
    if (SUCCEEDED(hr))
    {
        ViewProjBuffer* pVP = (ViewProjBuffer*)mappedSky.pData;
        XMStoreFloat4x4((XMFLOAT4X4*)&pVP->vp, XMMatrixTranspose(vpSky));
        g_pDeviceContext->Unmap(g_pViewProjBuffer, 0);
    }

    g_pDeviceContext->OMSetDepthStencilState(g_pSkyboxDepthState, 0);
    g_pDeviceContext->RSSetState(g_pCullNoneRS);

    g_pDeviceContext->VSSetShader(g_pSkyboxVS, nullptr, 0);
    g_pDeviceContext->PSSetShader(g_pSkyboxPS, nullptr, 0);
    g_pDeviceContext->IASetInputLayout(g_pSkyboxInputLayout);

    ID3D11Buffer* vbSky[] = { g_pSkyboxVertexBuffer };
    g_pDeviceContext->IASetVertexBuffers(0, 1, vbSky, &stride, &offset);
    g_pDeviceContext->IASetIndexBuffer(g_pSkyboxIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
    g_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11ShaderResourceView* skySRV[] = { g_pCubemapView };
    g_pDeviceContext->PSSetShaderResources(1, 1, skySRV);

    ID3D11Buffer* cbsSky[] = { nullptr, g_pViewProjBuffer };
    g_pDeviceContext->VSSetConstantBuffers(0, 2, cbsSky);

    g_pDeviceContext->DrawIndexed(36, 0, 0);

    // TRANSPARENT OBJECTS
    std::vector<TransparentObject> transparentObjects;
    transparentObjects.push_back({
        XMMatrixScaling(1.2f, 1.2f, 0.08f) * XMMatrixTranslation(-0.8f, 0.0f, 1.2f),
        XMFLOAT4(1.0f, 0.2f, 0.2f, 0.45f),
        XMFLOAT3(-0.8f, 0.0f, 1.2f),
        0.0f
        });

    transparentObjects.push_back({
        XMMatrixScaling(1.2f, 1.2f, 0.08f) * XMMatrixRotationY(angle * 0.5f) * XMMatrixTranslation(0.6f, 0.2f, -0.3f),
        XMFLOAT4(0.2f, 0.8f, 1.0f, 0.45f),
        XMFLOAT3(0.6f, 0.2f, -0.3f),
        0.0f
        });

    for (auto& obj : transparentObjects)
    {
        float dx = obj.center.x - camX;
        float dy = obj.center.y - camY;
        float dz = obj.center.z - camZ;
        obj.distanceToCamera = dx * dx + dy * dy + dz * dz;
    }

    std::sort(transparentObjects.begin(), transparentObjects.end(),
        [](const TransparentObject& a, const TransparentObject& b)
        {
            return a.distanceToCamera > b.distanceToCamera; // far -> near
        });

    hr = g_pDeviceContext->Map(g_pViewProjBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        ViewProjBuffer* pVP = (ViewProjBuffer*)mapped.pData;
        XMStoreFloat4x4((XMFLOAT4X4*)&pVP->vp, XMMatrixTranspose(vp));
        g_pDeviceContext->Unmap(g_pViewProjBuffer, 0);
    }

    FLOAT blendFactor[4] = { 0,0,0,0 };
    g_pDeviceContext->OMSetBlendState(g_pTransparentBlendState, blendFactor, 0xFFFFFFFF);
    g_pDeviceContext->OMSetDepthStencilState(g_pTransparentDepthState, 0);
    g_pDeviceContext->RSSetState(g_pCullBackRS);

    g_pDeviceContext->VSSetShader(g_pTransparentVS, nullptr, 0);
    g_pDeviceContext->PSSetShader(g_pTransparentPS, nullptr, 0);
    g_pDeviceContext->IASetInputLayout(g_pTransparentInputLayout);

    g_pDeviceContext->IASetVertexBuffers(0, 1, vbCube, &stride, &offset);
    g_pDeviceContext->IASetIndexBuffer(g_pIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
    g_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (const auto& obj : transparentObjects)
    {
        TransparentBuffer tb = {};
        XMStoreFloat4x4((XMFLOAT4X4*)&tb.model, XMMatrixTranspose(obj.model));
        tb.color = obj.color;
        g_pDeviceContext->UpdateSubresource(g_pTransparentBuffer, 0, nullptr, &tb, 0, 0);

        ID3D11Buffer* cbsTransparent[] = { g_pTransparentBuffer, g_pViewProjBuffer };
        g_pDeviceContext->VSSetConstantBuffers(0, 2, cbsTransparent);

        g_pDeviceContext->DrawIndexed(36, 0, 0);
    }

    g_pDeviceContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    g_pSwapChain->Present(1, 0);
}

// Resize
void OnResize(UINT newWidth, UINT newHeight)
{
    if (!g_pSwapChain || !g_pDevice || !g_pDeviceContext)
        return;

    g_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);

    SAFE_RELEASE(g_pBackBufferRTV);
    SAFE_RELEASE(g_pDepthStencilView);

    HRESULT hr = g_pSwapChain->ResizeBuffers(2, newWidth, newHeight, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return;

    ID3D11Texture2D* pBackBuffer = nullptr;
    hr = g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    if (FAILED(hr)) return;

    hr = g_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pBackBufferRTV);
    pBackBuffer->Release();
    if (FAILED(hr)) return;

    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = newWidth;
    depthDesc.Height = newHeight;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ID3D11Texture2D* pDepthStencil = nullptr;
    hr = g_pDevice->CreateTexture2D(&depthDesc, nullptr, &pDepthStencil);
    if (FAILED(hr)) return;

    hr = g_pDevice->CreateDepthStencilView(pDepthStencil, nullptr, &g_pDepthStencilView);
    pDepthStencil->Release();
    if (FAILED(hr)) return;

    g_ClientWidth = newWidth;
    g_ClientHeight = newHeight;
}

// Cleanup
void CleanupDirectX()
{
    if (g_pDeviceContext)
        g_pDeviceContext->ClearState();

    SAFE_RELEASE(g_pTransparentBlendState);
    SAFE_RELEASE(g_pTransparentDepthState);
    SAFE_RELEASE(g_pOpaqueDepthState);
    SAFE_RELEASE(g_pSkyboxDepthState);
    SAFE_RELEASE(g_pCullBackRS);
    SAFE_RELEASE(g_pCullNoneRS);

    SAFE_RELEASE(g_pSampler);
    SAFE_RELEASE(g_pTextureView);
    SAFE_RELEASE(g_pTexture);
    SAFE_RELEASE(g_pCubemapView);
    SAFE_RELEASE(g_pCubemapTexture);

    SAFE_RELEASE(g_pModelBuffer);
    SAFE_RELEASE(g_pViewProjBuffer);
    SAFE_RELEASE(g_pTransparentBuffer);

    SAFE_RELEASE(g_pInputLayout);
    SAFE_RELEASE(g_pVertexShader);
    SAFE_RELEASE(g_pPixelShader);

    SAFE_RELEASE(g_pSkyboxInputLayout);
    SAFE_RELEASE(g_pSkyboxVS);
    SAFE_RELEASE(g_pSkyboxPS);

    SAFE_RELEASE(g_pTransparentInputLayout);
    SAFE_RELEASE(g_pTransparentVS);
    SAFE_RELEASE(g_pTransparentPS);

    SAFE_RELEASE(g_pIndexBuffer);
    SAFE_RELEASE(g_pVertexBuffer);
    SAFE_RELEASE(g_pSkyboxIndexBuffer);
    SAFE_RELEASE(g_pSkyboxVertexBuffer);

    SAFE_RELEASE(g_pBackBufferRTV);
    SAFE_RELEASE(g_pSwapChain);
    SAFE_RELEASE(g_pDepthStencilView);

#ifdef _DEBUG
    if (g_pDevice)
    {
        ID3D11Debug* pDebug = nullptr;
        if (SUCCEEDED(g_pDevice->QueryInterface(__uuidof(ID3D11Debug), (void**)&pDebug)))
        {
            pDebug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
            pDebug->Release();
        }
    }
#endif

    SAFE_RELEASE(g_pDeviceContext);
    SAFE_RELEASE(g_pDevice);
}
