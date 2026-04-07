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
#include <cstdint>
#include <cstring>

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

// DDS helpers
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

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }

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
    desc.mipPitches.clear();
    desc.mipOffsets.clear();

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

bool CreateTextureFromDesc(ID3D11Device* device, const TextureDesc& texDesc, ID3D11Texture2D** ppTex, ID3D11ShaderResourceView** ppSRV)
{
    if (!device || !ppTex || !ppSRV) return false;

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

    std::vector<D3D11_SUBRESOURCE_DATA> texData(texDesc.mipmapsCount);

    if (!texDesc.mipOffsets.empty())
    {
        for (UINT32 mip = 0; mip < texDesc.mipmapsCount; ++mip)
        {
            texData[mip].pSysMem = reinterpret_cast<const BYTE*>(texDesc.pData) + texDesc.mipOffsets[mip];
            texData[mip].SysMemPitch = texDesc.mipPitches[mip];
            texData[mip].SysMemSlicePitch = 0;
        }
    }
    else
    {
        texData[0].pSysMem = texDesc.pData;
        texData[0].SysMemPitch = texDesc.pitch;
        texData[0].SysMemSlicePitch = 0;
    }

    HRESULT hr = device->CreateTexture2D(&tex2DDesc, texData.data(), ppTex);
    if (FAILED(hr) || !*ppTex)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.fmt;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = texDesc.mipmapsCount;
    srvDesc.Texture2D.MostDetailedMip = 0;

    hr = device->CreateShaderResourceView(*ppTex, &srvDesc, ppSRV);
    if (FAILED(hr) || !*ppSRV)
    {
        SAFE_RELEASE(*ppTex);
        return false;
    }

    return true;
}

// Globals
HWND g_hWnd = nullptr;

ID3D11Device* g_pDevice = nullptr;
ID3D11DeviceContext* g_pDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_pBackBufferRTV = nullptr;
ID3D11DepthStencilView* g_pDepthStencilView = nullptr;
ID3D11Texture2D* g_pDepthStencilTexture = nullptr;

UINT g_ClientWidth = 1280;
UINT g_ClientHeight = 720;

struct LitVertex
{
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz;
    float u, v;
};

struct SkyboxVertex
{
    float x, y, z;
    float u, v;
};

ID3D11Buffer* g_pVertexBuffer = nullptr;
ID3D11Buffer* g_pIndexBuffer = nullptr;
ID3D11Buffer* g_pSkyboxVertexBuffer = nullptr;
ID3D11Buffer* g_pSkyboxIndexBuffer = nullptr;

ID3D11VertexShader* g_pVertexShader = nullptr;
ID3D11PixelShader* g_pPixelShader = nullptr;
ID3D11InputLayout* g_pInputLayout = nullptr;

ID3D11VertexShader* g_pSkyboxVS = nullptr;
ID3D11PixelShader* g_pSkyboxPS = nullptr;
ID3D11InputLayout* g_pSkyboxInputLayout = nullptr;

ID3D11VertexShader* g_pTransparentVS = nullptr;
ID3D11PixelShader* g_pTransparentPS = nullptr;
ID3D11InputLayout* g_pTransparentInputLayout = nullptr;

ID3D11VertexShader* g_pPostVS = nullptr;
ID3D11PixelShader* g_pPostPS = nullptr;
ID3D11ComputeShader* g_pCullCS = nullptr;

struct ViewProjBuffer
{
    XMMATRIX vp;
};

struct LightData
{
    XMFLOAT4 position;
    XMFLOAT4 color;
};

struct SceneBuffer
{
    XMFLOAT4 cameraPos;
    XMFLOAT4 ambientColor;
    LightData lights[4];
    XMINT4 lightCount;
};

struct TransparentBuffer
{
    XMMATRIX model;
    XMFLOAT4 color;
};

struct PostProcessBuffer
{
    XMINT4 mode;
};

static const UINT MAX_INSTANCES = 256;
static const UINT MAX_VISIBLE_INSTANCES = 256;
static const UINT QUERY_COUNT = 10;

struct InstanceDataGPU
{
    XMFLOAT4X4 model;
    XMFLOAT4X4 normalMatrix;
    XMFLOAT4 params;
    XMFLOAT4 posAngle;
};

struct VisibleIdGPU
{
    UINT id;
    UINT pad0;
    UINT pad1;
    UINT pad2;
};

struct CubeInstanceCPU
{
    XMFLOAT3 basePos;
    float scale;
    float rotSpeed;
    UINT textureId;
    bool hasNormalMap;
};

ID3D11Buffer* g_pViewProjBuffer = nullptr;
ID3D11Buffer* g_pSceneBuffer = nullptr;
ID3D11Buffer* g_pTransparentBuffer = nullptr;
ID3D11Buffer* g_pPostProcessBuffer = nullptr;
ID3D11Buffer* g_pInstanceBuffer = nullptr;

ID3D11Texture2D* g_pNormalTexture = nullptr;
ID3D11ShaderResourceView* g_pNormalTextureView = nullptr;

ID3D11Texture2D* g_pTextureArray = nullptr;
ID3D11ShaderResourceView* g_pTextureArrayView = nullptr;

ID3D11Texture2D* g_pCubemapTexture = nullptr;
ID3D11ShaderResourceView* g_pCubemapView = nullptr;

ID3D11SamplerState* g_pSampler = nullptr;

ID3D11BlendState* g_pTransparentBlendState = nullptr;
ID3D11DepthStencilState* g_pTransparentDepthState = nullptr;
ID3D11DepthStencilState* g_pOpaqueDepthState = nullptr;
ID3D11DepthStencilState* g_pSkyboxDepthState = nullptr;
ID3D11RasterizerState* g_pCullBackRS = nullptr;
ID3D11RasterizerState* g_pCullNoneRS = nullptr;

ID3D11Texture2D* g_pSceneColorTex = nullptr;
ID3D11RenderTargetView* g_pSceneColorRTV = nullptr;
ID3D11ShaderResourceView* g_pSceneColorSRV = nullptr;

// GPU frustum culling + queries
struct Plane
{
    XMFLOAT4 p;
};

struct AABBPoint
{
    XMFLOAT4 v;
};

struct FrustumBufferCPU
{
    XMFLOAT4 frustum[6];
};

struct CullParamsCPU
{
    XMUINT4 numShapes;
    AABBPoint bbMin[MAX_INSTANCES];
    AABBPoint bbMax[MAX_INSTANCES];
};

ID3D11Buffer* g_pFrustumBuffer = nullptr;
ID3D11Buffer* g_pCullParamsBuffer = nullptr;
ID3D11Buffer* g_pVisibleIdsGPU = nullptr;
ID3D11UnorderedAccessView* g_pVisibleIdsUAV = nullptr;
ID3D11ShaderResourceView* g_pVisibleIdsSRV = nullptr;
ID3D11Buffer* g_pIndirectArgsSrc = nullptr;
ID3D11UnorderedAccessView* g_pIndirectArgsUAV = nullptr;
ID3D11Buffer* g_pIndirectArgs = nullptr;
ID3D11Query* g_pPipelineQueries[QUERY_COUNT] = {};
UINT g_CurFrame = 0;
UINT g_LastCompletedFrame = 0;
UINT g_GPUVisibleInstances = 0;

float g_CameraYaw = 0.0f;
float g_CameraPitch = 0.3f;
float g_CameraDist = 10.0f;
bool g_KeyLeft = false, g_KeyRight = false, g_KeyUp = false, g_KeyDown = false;

bool g_UseGPUCull = true;

double g_LastTime = 0.0;
int g_PostEffectMode = 2;

struct TransparentObject
{
    XMMATRIX model;
    XMFLOAT4 color;
    XMFLOAT3 center;
    float distanceToCamera;
};

std::vector<CubeInstanceCPU> g_OpaqueCubes;

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
bool InitDirectX();
void CreateCubeResources();
bool CompileShaders();
bool LoadTextures();
bool CreateRenderStates();
bool CreatePostProcessResources(UINT width, UINT height);
bool CreateConstantBuffers();
bool CreateGPUCullingResources();
void CreateOpaqueInstances();
void CleanupDirectX();
void RenderFrame();
void OnResize(UINT newWidth, UINT newHeight);
void UpdateCamera(double deltaTime);

Plane NormalizePlane(const Plane& in);
void ExtractFrustumPlanes(Plane planes[6], const XMMATRIX& vp);
void ComputeWorldAABB(const XMMATRIX& model, XMFLOAT3& outMin, XMFLOAT3& outMax);
void RunGPUFrustumCulling(const Plane planes[6], const std::vector<InstanceDataGPU>& instanceData);
void ReadGPUQueries();

// Entry point
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

    g_hWnd = CreateWindowW(
        wc.lpszClassName,
        L"Thu Hoai - Instancing + GPU Frustum Culling + Post Process",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        winWidth, winHeight,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd)
    {
        MessageBoxW(nullptr, L"CreateWindow failed", L"Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 0;
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    if (!InitDirectX() ||
        !(CreateCubeResources(), true) ||
        !CompileShaders() ||
        !LoadTextures() ||
        !CreateRenderStates() ||
        !CreateConstantBuffers() ||
        !CreateGPUCullingResources() ||
        !CreatePostProcessResources(g_ClientWidth, g_ClientHeight))
    {
        CleanupDirectX();
        DestroyWindow(g_hWnd);
        CoUninitialize();
        return -1;
    }

    CreateOpaqueInstances();

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
        if (wParam == '1') g_PostEffectMode = 0;
        if (wParam == '2') g_PostEffectMode = 1;
        if (wParam == '3') g_PostEffectMode = 2;
        if (wParam == '4') g_PostEffectMode = 3;
        if (wParam == 'G') g_UseGPUCull = !g_UseGPUCull;
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

    hr = g_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pBackBufferRTV);
    pBackBuffer->Release();
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

    hr = g_pDevice->CreateTexture2D(&depthDesc, nullptr, &g_pDepthStencilTexture);
    if (FAILED(hr)) return false;

    hr = g_pDevice->CreateDepthStencilView(g_pDepthStencilTexture, nullptr, &g_pDepthStencilView);
    if (FAILED(hr)) return false;

    return true;
}

// Geometry
void CreateCubeResources()
{
    const LitVertex cubeVertices[] =
    {
        { -0.5f, -0.5f, -0.5f,  0, 0,-1,  -1, 0, 0,  0,1 },
        {  0.5f, -0.5f, -0.5f,  0, 0,-1,  -1, 0, 0,  1,1 },
        {  0.5f,  0.5f, -0.5f,  0, 0,-1,  -1, 0, 0,  1,0 },
        { -0.5f,  0.5f, -0.5f,  0, 0,-1,  -1, 0, 0,  0,0 },

        { -0.5f, -0.5f,  0.5f,  0, 0, 1,   1, 0, 0,  0,1 },
        {  0.5f, -0.5f,  0.5f,  0, 0, 1,   1, 0, 0,  1,1 },
        {  0.5f,  0.5f,  0.5f,  0, 0, 1,   1, 0, 0,  1,0 },
        { -0.5f,  0.5f,  0.5f,  0, 0, 1,   1, 0, 0,  0,0 },

        { -0.5f, -0.5f,  0.5f, -1, 0, 0,   0, 0,-1,  0,1 },
        { -0.5f, -0.5f, -0.5f, -1, 0, 0,   0, 0,-1,  1,1 },
        { -0.5f,  0.5f, -0.5f, -1, 0, 0,   0, 0,-1,  1,0 },
        { -0.5f,  0.5f,  0.5f, -1, 0, 0,   0, 0,-1,  0,0 },

        {  0.5f, -0.5f, -0.5f,  1, 0, 0,   0, 0, 1,  0,1 },
        {  0.5f, -0.5f,  0.5f,  1, 0, 0,   0, 0, 1,  1,1 },
        {  0.5f,  0.5f,  0.5f,  1, 0, 0,   0, 0, 1,  1,0 },
        {  0.5f,  0.5f, -0.5f,  1, 0, 0,   0, 0, 1,  0,0 },

        { -0.5f,  0.5f, -0.5f,  0, 1, 0,   1, 0, 0,  0,1 },
        {  0.5f,  0.5f, -0.5f,  0, 1, 0,   1, 0, 0,  1,1 },
        {  0.5f,  0.5f,  0.5f,  0, 1, 0,   1, 0, 0,  1,0 },
        { -0.5f,  0.5f,  0.5f,  0, 1, 0,   1, 0, 0,  0,0 },

        { -0.5f, -0.5f,  0.5f,  0,-1, 0,   1, 0, 0,  0,1 },
        {  0.5f, -0.5f,  0.5f,  0,-1, 0,   1, 0, 0,  1,1 },
        {  0.5f, -0.5f, -0.5f,  0,-1, 0,   1, 0, 0,  1,0 },
        { -0.5f, -0.5f, -0.5f,  0,-1, 0,   1, 0, 0,  0,0 }
    };

    const USHORT cubeIndices[] =
    {
        0, 2, 1,  0, 3, 2,
        4, 5, 6,  4, 6, 7,
        8,10, 9,  8,11,10,
        12,14,13, 12,15,14,
        16,18,17, 16,19,18,
        20,22,21, 20,23,22
    };

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(cubeVertices);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data = { cubeVertices };

    HRESULT hr = g_pDevice->CreateBuffer(&desc, &data, &g_pVertexBuffer);
    if (FAILED(hr)) return;

    desc = {};
    desc.ByteWidth = sizeof(cubeIndices);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = cubeIndices;

    hr = g_pDevice->CreateBuffer(&desc, &data, &g_pIndexBuffer);
    if (FAILED(hr)) return;

    const SkyboxVertex skyboxVertices[] =
    {
        {-10, -10, -10, 0, 0}, { 10, -10, -10, 0, 0}, { 10,  10, -10, 0, 0}, {-10,  10, -10, 0, 0},
        {-10, -10,  10, 0, 0}, { 10, -10,  10, 0, 0}, { 10,  10,  10, 0, 0}, {-10,  10,  10, 0, 0}
    };

    const USHORT skyboxIndices[] =
    {
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

    desc = {};
    desc.ByteWidth = sizeof(skyboxIndices);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = skyboxIndices;
    hr = g_pDevice->CreateBuffer(&desc, &data, &g_pSkyboxIndexBuffer);
}

// Shaders
bool CompileShaders()
{
    const char* litVS = R"(
cbuffer ViewProjBuffer : register(b1)
{
    float4x4 vp;
};

struct InstanceData
{
    float4x4 model;
    float4x4 normalMatrix;
    float4 params;
    float4 posAngle;
};

cbuffer InstanceBuffer : register(b3)
{
    InstanceData instData[256];
};

StructuredBuffer<uint4> visibleIds : register(t6);

struct VSInput
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;
    uint instanceId : SV_InstanceID;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float3 worldPos : POSITION1;
    float3 normalW : NORMAL;
    float3 tangentW : TANGENT;
    float2 uv : TEXCOORD;
    nointerpolation uint instanceId : INST_ID;
};

VSOutput vs(VSInput v)
{
    VSOutput o;

    uint idx = visibleIds[v.instanceId].x;
    float4x4 model = instData[idx].model;
    float4x4 normalMatrix = instData[idx].normalMatrix;

    float4 worldPos = mul(float4(v.pos, 1.0f), model);
    o.pos = mul(worldPos, vp);
    o.worldPos = worldPos.xyz;

    o.normalW = normalize(mul(float4(v.normal, 0.0f), normalMatrix).xyz);
    o.tangentW = normalize(mul(float4(v.tangent, 0.0f), normalMatrix).xyz);
    o.uv = v.uv;
    o.instanceId = idx;

    return o;
}
)";

    const char* litPS = R"(
Texture2DArray colorTexture  : register(t0);
Texture2D normalTexture      : register(t1);
SamplerState colorSampler    : register(s0);

struct LightData
{
    float4 position;
    float4 color;
};

cbuffer SceneBuffer : register(b2)
{
    float4 cameraPos;
    float4 ambientColor;
    LightData lights[4];
    int4 lightCount;
};

struct InstanceData
{
    float4x4 model;
    float4x4 normalMatrix;
    float4 params;
    float4 posAngle;
};

cbuffer InstanceBuffer : register(b3)
{
    InstanceData instData[256];
};

struct VSOutput
{
    float4 pos : SV_Position;
    float3 worldPos : POSITION1;
    float3 normalW : NORMAL;
    float3 tangentW : TANGENT;
    float2 uv : TEXCOORD;
    nointerpolation uint instanceId : INST_ID;
};

float4 ps(VSOutput pixel) : SV_Target
{
    uint idx = pixel.instanceId;

    float shininess = instData[idx].params.x;
    float texId = instData[idx].params.z;
    float hasNM = instData[idx].params.w;

    float3 albedo = colorTexture.Sample(colorSampler, float3(pixel.uv, texId)).rgb;

    float3 N = normalize(pixel.normalW);
    float3 T = normalize(pixel.tangentW);
    float3 B = normalize(cross(N, T));

    float3 normal = N;
    if (hasNM > 0.5f)
    {
        float3 nMap = normalTexture.Sample(colorSampler, pixel.uv).xyz;
        nMap = normalize(nMap * 2.0f - 1.0f);
        normal = normalize(nMap.x * T + nMap.y * B + nMap.z * N);
    }

    float3 viewDir = normalize(cameraPos.xyz - pixel.worldPos);

    float3 finalColor = ambientColor.rgb * albedo;

    [unroll]
    for (int i = 0; i < lightCount.x; ++i)
    {
        float3 lightVec = lights[i].position.xyz - pixel.worldPos;
        float dist = length(lightVec);
        float3 lightDir = lightVec / max(dist, 0.0001f);

        float attenuation = 1.0f / (1.0f + 0.15f * dist + 0.08f * dist * dist);
        float diffuse = max(dot(normal, lightDir), 0.0f);

        float3 reflectDir = reflect(-lightDir, normal);
        float spec = pow(max(dot(viewDir, reflectDir), 0.0f), shininess);

        float3 lightColor = lights[i].color.rgb;
        finalColor += albedo * (0.15f + diffuse) * lightColor * attenuation;
        finalColor += spec * lightColor * attenuation * 0.45f;
    }

    return float4(saturate(finalColor), 1.0f);
}
)";

    const char* skyboxVS = R"(
cbuffer ViewProjBuffer : register(b1)
{
    float4x4 vp;
};

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
TextureCube skyboxTexture : register(t5);
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
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
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

    const char* postVS = R"(
struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOutput vs(uint vertexId : SV_VertexID)
{
    VSOutput o;

    float2 pos[6] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0),
        float2(-1.0, -1.0),
        float2( 1.0,  1.0),
        float2( 1.0, -1.0)
    };

    float2 p = pos[vertexId];
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return o;
}
)";

    const char* postPS = R"(
Texture2D sceneTexture : register(t0);
SamplerState sceneSampler : register(s0);

cbuffer PostProcessBuffer : register(b0)
{
    int4 effectMode;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 ps(VSOutput pixel) : SV_Target0
{
    float3 color = sceneTexture.Sample(sceneSampler, pixel.uv).rgb;

    if (effectMode.x == 1)
    {
        float gray = dot(color, float3(0.299, 0.587, 0.114));
        color = float3(gray, gray, gray);
    }
    else if (effectMode.x == 2)
    {
        float3 sepia;
        sepia.r = dot(color, float3(0.393, 0.769, 0.189));
        sepia.g = dot(color, float3(0.349, 0.686, 0.168));
        sepia.b = dot(color, float3(0.272, 0.534, 0.131));
        color = lerp(color, sepia, 0.75);
    }
    else if (effectMode.x == 3)
    {
        color *= 1.18;
    }

    return float4(saturate(color), 1.0);
}
)";

    const char* cullCS = R"(
cbuffer FrustumBuffer : register(b0)
{
    float4 frustum[6];
};

cbuffer CullParams : register(b1)
{
    uint4 numShapes;
    float4 bbMin[256];
    float4 bbMax[256];
};

RWStructuredBuffer<uint> indirectArgs : register(u0);
RWStructuredBuffer<uint4> objectIds   : register(u1);

bool IsBoxInside(in float4 fr[6], in float3 bmin, in float3 bmax)
{
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float3 n = fr[i].xyz;
        float4 p = float4(
            n.x < 0 ? bmin.x : bmax.x,
            n.y < 0 ? bmin.y : bmax.y,
            n.z < 0 ? bmin.z : bmax.z,
            1.0f
        );

        float s = dot(p, fr[i]);
        if (s < 0.0f)
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void cs(uint3 globalThreadId : SV_DispatchThreadID)
{
    uint idx = globalThreadId.x;
    if (idx >= numShapes.x)
        return;

    if (IsBoxInside(frustum, bbMin[idx].xyz, bbMax[idx].xyz))
    {
        uint outId = 0;
        InterlockedAdd(indirectArgs[1], 1, outId);
        objectIds[outId] = uint4(idx, 0, 0, 0);
    }
}
)";

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr;
    ID3DBlob* pVsBlob = nullptr;
    ID3DBlob* pPsBlob = nullptr;
    ID3DBlob* pCsBlob = nullptr;
    ID3DBlob* pErrorBlob = nullptr;

    D3D11_INPUT_ELEMENT_DESC litLayout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };

    D3D11_INPUT_ELEMENT_DESC skyboxLayout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };

    auto PrintError = [&](ID3DBlob* blob)
        {
            if (blob)
                OutputDebugStringA((const char*)blob->GetBufferPointer());
        };

    hr = D3DCompile(litVS, strlen(litVS), nullptr, nullptr, nullptr, "vs", "vs_5_0", flags, 0, &pVsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        PrintError(pErrorBlob);
        SAFE_RELEASE(pErrorBlob);
        return false;
    }
    hr = g_pDevice->CreateVertexShader(pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), nullptr, &g_pVertexShader);
    if (FAILED(hr)) return false;
    hr = g_pDevice->CreateInputLayout(litLayout, 4, pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), &g_pInputLayout);
    if (FAILED(hr)) return false;
    SAFE_RELEASE(pVsBlob);

    hr = D3DCompile(litPS, strlen(litPS), nullptr, nullptr, nullptr, "ps", "ps_5_0", flags, 0, &pPsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        PrintError(pErrorBlob);
        SAFE_RELEASE(pErrorBlob);
        return false;
    }
    hr = g_pDevice->CreatePixelShader(pPsBlob->GetBufferPointer(), pPsBlob->GetBufferSize(), nullptr, &g_pPixelShader);
    if (FAILED(hr)) return false;
    SAFE_RELEASE(pPsBlob);

    hr = D3DCompile(cullCS, strlen(cullCS), nullptr, nullptr, nullptr, "cs", "cs_5_0", flags, 0, &pCsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        PrintError(pErrorBlob);
        SAFE_RELEASE(pErrorBlob);
        return false;
    }
    hr = g_pDevice->CreateComputeShader(pCsBlob->GetBufferPointer(), pCsBlob->GetBufferSize(), nullptr, &g_pCullCS);
    if (FAILED(hr)) return false;
    SAFE_RELEASE(pCsBlob);

    hr = D3DCompile(skyboxVS, strlen(skyboxVS), nullptr, nullptr, nullptr, "vs", "vs_5_0", flags, 0, &pVsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        PrintError(pErrorBlob);
        SAFE_RELEASE(pErrorBlob);
        return false;
    }
    hr = g_pDevice->CreateVertexShader(pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), nullptr, &g_pSkyboxVS);
    if (FAILED(hr)) return false;
    hr = g_pDevice->CreateInputLayout(skyboxLayout, 2, pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), &g_pSkyboxInputLayout);
    if (FAILED(hr)) return false;
    SAFE_RELEASE(pVsBlob);

    hr = D3DCompile(skyboxPS, strlen(skyboxPS), nullptr, nullptr, nullptr, "ps", "ps_5_0", flags, 0, &pPsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        PrintError(pErrorBlob);
        SAFE_RELEASE(pErrorBlob);
        return false;
    }
    hr = g_pDevice->CreatePixelShader(pPsBlob->GetBufferPointer(), pPsBlob->GetBufferSize(), nullptr, &g_pSkyboxPS);
    if (FAILED(hr)) return false;
    SAFE_RELEASE(pPsBlob);

    hr = D3DCompile(transparentVS, strlen(transparentVS), nullptr, nullptr, nullptr, "vs", "vs_5_0", flags, 0, &pVsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        PrintError(pErrorBlob);
        SAFE_RELEASE(pErrorBlob);
        return false;
    }
    hr = g_pDevice->CreateVertexShader(pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), nullptr, &g_pTransparentVS);
    if (FAILED(hr)) return false;
    hr = g_pDevice->CreateInputLayout(litLayout, 4, pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), &g_pTransparentInputLayout);
    if (FAILED(hr)) return false;
    SAFE_RELEASE(pVsBlob);

    hr = D3DCompile(transparentPS, strlen(transparentPS), nullptr, nullptr, nullptr, "ps", "ps_5_0", flags, 0, &pPsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        PrintError(pErrorBlob);
        SAFE_RELEASE(pErrorBlob);
        return false;
    }
    hr = g_pDevice->CreatePixelShader(pPsBlob->GetBufferPointer(), pPsBlob->GetBufferSize(), nullptr, &g_pTransparentPS);
    if (FAILED(hr)) return false;
    SAFE_RELEASE(pPsBlob);

    hr = D3DCompile(postVS, strlen(postVS), nullptr, nullptr, nullptr, "vs", "vs_5_0", flags, 0, &pVsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        PrintError(pErrorBlob);
        SAFE_RELEASE(pErrorBlob);
        return false;
    }
    hr = g_pDevice->CreateVertexShader(pVsBlob->GetBufferPointer(), pVsBlob->GetBufferSize(), nullptr, &g_pPostVS);
    if (FAILED(hr)) return false;
    SAFE_RELEASE(pVsBlob);

    hr = D3DCompile(postPS, strlen(postPS), nullptr, nullptr, nullptr, "ps", "ps_5_0", flags, 0, &pPsBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        PrintError(pErrorBlob);
        SAFE_RELEASE(pErrorBlob);
        return false;
    }
    hr = g_pDevice->CreatePixelShader(pPsBlob->GetBufferPointer(), pPsBlob->GetBufferSize(), nullptr, &g_pPostPS);
    if (FAILED(hr)) return false;
    SAFE_RELEASE(pPsBlob);

    SAFE_RELEASE(pErrorBlob);
    return true;
}

// Textures
bool CreateTexture2DArrayFromDDS(const wchar_t* file0, const wchar_t* file1, ID3D11Texture2D** ppTex, ID3D11ShaderResourceView** ppSRV)
{
    if (!ppTex || !ppSRV) return false;

    TextureDesc texDesc[2];
    if (!LoadDDS(file0, texDesc[0])) return false;

    if (!LoadDDS(file1, texDesc[1]))
    {
        free(texDesc[0].pData);
        return false;
    }

    if (texDesc[0].fmt != texDesc[1].fmt ||
        texDesc[0].width != texDesc[1].width ||
        texDesc[0].height != texDesc[1].height ||
        texDesc[0].mipmapsCount != texDesc[1].mipmapsCount)
    {
        free(texDesc[0].pData);
        free(texDesc[1].pData);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = texDesc[0].width;
    desc.Height = texDesc[0].height;
    desc.MipLevels = texDesc[0].mipmapsCount;
    desc.ArraySize = 2;
    desc.Format = texDesc[0].fmt;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<D3D11_SUBRESOURCE_DATA> data(desc.MipLevels * 2);

    for (UINT slice = 0; slice < 2; ++slice)
    {
        for (UINT mip = 0; mip < desc.MipLevels; ++mip)
        {
            UINT idx = slice * desc.MipLevels + mip;
            data[idx].pSysMem = reinterpret_cast<const BYTE*>(texDesc[slice].pData) + texDesc[slice].mipOffsets[mip];
            data[idx].SysMemPitch = texDesc[slice].mipPitches[mip];
            data[idx].SysMemSlicePitch = 0;
        }
    }

    HRESULT hr = g_pDevice->CreateTexture2D(&desc, data.data(), ppTex);
    if (FAILED(hr))
    {
        free(texDesc[0].pData);
        free(texDesc[1].pData);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = desc.MipLevels;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = 2;

    hr = g_pDevice->CreateShaderResourceView(*ppTex, &srvDesc, ppSRV);

    free(texDesc[0].pData);
    free(texDesc[1].pData);

    if (FAILED(hr))
    {
        SAFE_RELEASE(*ppTex);
        return false;
    }

    return true;
}

bool LoadTextures()
{
    if (!CreateTexture2DArrayFromDDS(L"Brick.dds", L"Kitty.dds", &g_pTextureArray, &g_pTextureArrayView))
    {
        MessageBoxA(NULL, "Failed to create texture array from Brick.dds and Kitty.dds", "Error", MB_OK);
        return false;
    }

    {
        TextureDesc texDesc;
        if (!LoadDDS(L"BrickNM.dds", texDesc))
        {
            MessageBoxA(NULL, "Failed to load BrickNM.dds", "Error", MB_OK);
            return false;
        }

        if (!CreateTextureFromDesc(g_pDevice, texDesc, &g_pNormalTexture, &g_pNormalTextureView))
        {
            free(texDesc.pData);
            MessageBoxA(NULL, "Failed to create normal map texture", "Error", MB_OK);
            return false;
        }

        free(texDesc.pData);
    }

    {
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_ANISOTROPIC;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampDesc.MinLOD = -FLT_MAX;
        sampDesc.MaxLOD = FLT_MAX;
        sampDesc.MaxAnisotropy = 16;
        sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampDesc.BorderColor[0] = 1.0f;
        sampDesc.BorderColor[1] = 1.0f;
        sampDesc.BorderColor[2] = 1.0f;
        sampDesc.BorderColor[3] = 1.0f;

        HRESULT hr = g_pDevice->CreateSamplerState(&sampDesc, &g_pSampler);
        if (FAILED(hr))
        {
            MessageBoxA(NULL, "CreateSampler failed", "Error", MB_OK);
            return false;
        }
    }

    {
        const std::wstring faceNames[6] =
        {
            L"Skybox/posx.png",
            L"Skybox/negx.png",
            L"Skybox/posy.png",
            L"Skybox/negy.png",
            L"Skybox/posz.png",
            L"Skybox/negz.png"
        };

        TextureDesc faceDescs[6] = {};
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
            return false;
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
                return false;
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

        HRESULT hr = g_pDevice->CreateTexture2D(&cubeDesc, initData, &g_pCubemapTexture);

        for (int i = 0; i < 6; ++i)
            free(faceDescs[i].pData);

        if (FAILED(hr) || !g_pCubemapTexture)
        {
            MessageBoxA(NULL, "CreateCubemapTexture failed", "Error", MB_OK);
            return false;
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
            return false;
        }
    }

    return true;
}

// Render states
bool CreateRenderStates()
{
    HRESULT hr;

    D3D11_DEPTH_STENCIL_DESC opaqueDesc = {};
    opaqueDesc.DepthEnable = TRUE;
    opaqueDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    opaqueDesc.DepthFunc = D3D11_COMPARISON_LESS;
    hr = g_pDevice->CreateDepthStencilState(&opaqueDesc, &g_pOpaqueDepthState);
    if (FAILED(hr)) return false;

    D3D11_DEPTH_STENCIL_DESC skyboxDesc = {};
    skyboxDesc.DepthEnable = TRUE;
    skyboxDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    skyboxDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = g_pDevice->CreateDepthStencilState(&skyboxDesc, &g_pSkyboxDepthState);
    if (FAILED(hr)) return false;

    D3D11_DEPTH_STENCIL_DESC transparentDesc = {};
    transparentDesc.DepthEnable = TRUE;
    transparentDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    transparentDesc.DepthFunc = D3D11_COMPARISON_LESS;
    hr = g_pDevice->CreateDepthStencilState(&transparentDesc, &g_pTransparentDepthState);
    if (FAILED(hr)) return false;

    D3D11_RASTERIZER_DESC rsBack = {};
    rsBack.FillMode = D3D11_FILL_SOLID;
    rsBack.CullMode = D3D11_CULL_BACK;
    rsBack.DepthClipEnable = TRUE;
    hr = g_pDevice->CreateRasterizerState(&rsBack, &g_pCullBackRS);
    if (FAILED(hr)) return false;

    D3D11_RASTERIZER_DESC rsNone = {};
    rsNone.FillMode = D3D11_FILL_SOLID;
    rsNone.CullMode = D3D11_CULL_NONE;
    rsNone.DepthClipEnable = TRUE;
    hr = g_pDevice->CreateRasterizerState(&rsNone, &g_pCullNoneRS);
    if (FAILED(hr)) return false;

    D3D11_BLEND_DESC blendDesc = {};
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

// Constant buffers
bool CreateConstantBuffers()
{
    HRESULT hr;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(ViewProjBuffer);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pViewProjBuffer);
    if (FAILED(hr)) return false;

    desc = {};
    desc.ByteWidth = sizeof(SceneBuffer);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pSceneBuffer);
    if (FAILED(hr)) return false;

    desc = {};
    desc.ByteWidth = sizeof(TransparentBuffer);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pTransparentBuffer);
    if (FAILED(hr)) return false;

    desc = {};
    desc.ByteWidth = sizeof(PostProcessBuffer);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pPostProcessBuffer);
    if (FAILED(hr)) return false;

    desc = {};
    desc.ByteWidth = sizeof(InstanceDataGPU) * MAX_INSTANCES;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pInstanceBuffer);
    if (FAILED(hr)) return false;

    return true;
}

// GPU culling resources
bool CreateGPUCullingResources()
{
    HRESULT hr;
    D3D11_BUFFER_DESC desc = {};

    // Frustum constant buffer
    desc.ByteWidth = sizeof(FrustumBufferCPU);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pFrustumBuffer);
    if (FAILED(hr)) return false;

    // Cull params constant buffer
    desc = {};
    desc.ByteWidth = sizeof(CullParamsCPU);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pCullParamsBuffer);
    if (FAILED(hr)) return false;

    // Visible IDs structured buffer: UAV + SRV
    desc = {};
    desc.ByteWidth = sizeof(VisibleIdGPU) * MAX_VISIBLE_INSTANCES;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(VisibleIdGPU);
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pVisibleIdsGPU);
    if (FAILED(hr)) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = MAX_VISIBLE_INSTANCES;
    uavDesc.Buffer.Flags = 0;
    hr = g_pDevice->CreateUnorderedAccessView(g_pVisibleIdsGPU, &uavDesc, &g_pVisibleIdsUAV);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = MAX_VISIBLE_INSTANCES;
    hr = g_pDevice->CreateShaderResourceView(g_pVisibleIdsGPU, &srvDesc, &g_pVisibleIdsSRV);
    if (FAILED(hr)) return false;

    // Indirect args source buffer: UAV-writable structured buffer of 5 UINTs
    desc = {};
    desc.ByteWidth = sizeof(UINT) * 5;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(UINT);
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pIndirectArgsSrc);
    if (FAILED(hr)) return false;

    uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = 5;
    uavDesc.Buffer.Flags = 0;
    hr = g_pDevice->CreateUnorderedAccessView(g_pIndirectArgsSrc, &uavDesc, &g_pIndirectArgsUAV);
    if (FAILED(hr)) return false;

    // Actual indirect draw args buffer
    desc = {};
    desc.ByteWidth = sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    desc.StructureByteStride = 0;
    hr = g_pDevice->CreateBuffer(&desc, nullptr, &g_pIndirectArgs);
    if (FAILED(hr)) return false;

    // Pipeline statistics queries
    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
    qd.MiscFlags = 0;

    for (UINT i = 0; i < QUERY_COUNT; ++i)
    {
        hr = g_pDevice->CreateQuery(&qd, &g_pPipelineQueries[i]);
        if (FAILED(hr)) return false;
    }

    return true;
}

// Postprocess RT
bool CreatePostProcessResources(UINT width, UINT height)
{
    SAFE_RELEASE(g_pSceneColorSRV);
    SAFE_RELEASE(g_pSceneColorRTV);
    SAFE_RELEASE(g_pSceneColorTex);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = g_pDevice->CreateTexture2D(&desc, nullptr, &g_pSceneColorTex);
    if (FAILED(hr)) return false;

    hr = g_pDevice->CreateRenderTargetView(g_pSceneColorTex, nullptr, &g_pSceneColorRTV);
    if (FAILED(hr)) return false;

    hr = g_pDevice->CreateShaderResourceView(g_pSceneColorTex, nullptr, &g_pSceneColorSRV);
    if (FAILED(hr)) return false;

    return true;
}

// Scene setup
void CreateOpaqueInstances()
{
    g_OpaqueCubes.clear();

    const int gridX = 12;
    const int gridZ = 12;
    const float spacing = 1.65f;

    for (int z = 0; z < gridZ; ++z)
    {
        for (int x = 0; x < gridX; ++x)
        {
            if (g_OpaqueCubes.size() >= MAX_INSTANCES)
                return;

            CubeInstanceCPU cube = {};
            cube.basePos = XMFLOAT3(
                (x - gridX / 2) * spacing,
                0.0f,
                (z - gridZ / 2) * spacing
            );
            cube.scale = 0.75f + 0.15f * float((x + z) % 3);
            cube.rotSpeed = 0.35f + 0.08f * float((x * 7 + z * 3) % 5);
            cube.textureId = ((x + z) % 2 == 0) ? 0u : 1u;
            cube.hasNormalMap = (cube.textureId == 0u);

            g_OpaqueCubes.push_back(cube);
        }
    }
}

void UpdateCamera(double deltaTime)
{
    float speed = 1.0f;
    if (g_KeyLeft)  g_CameraYaw -= speed * (float)deltaTime;
    if (g_KeyRight) g_CameraYaw += speed * (float)deltaTime;
    if (g_KeyUp)    g_CameraPitch += speed * (float)deltaTime;
    if (g_KeyDown)  g_CameraPitch -= speed * (float)deltaTime;

    if (g_CameraPitch > 1.5f)  g_CameraPitch = 1.5f;
    if (g_CameraPitch < -1.5f) g_CameraPitch = -1.5f;
}

Plane NormalizePlane(const Plane& in)
{
    Plane out = in;
    float len = sqrtf(in.p.x * in.p.x + in.p.y * in.p.y + in.p.z * in.p.z);
    if (len > 0.00001f)
    {
        out.p.x /= len;
        out.p.y /= len;
        out.p.z /= len;
        out.p.w /= len;
    }
    return out;
}

void ExtractFrustumPlanes(Plane planes[6], const XMMATRIX& vp)
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, vp);

    planes[0].p = XMFLOAT4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41);
    planes[1].p = XMFLOAT4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41);
    planes[2].p = XMFLOAT4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42);
    planes[3].p = XMFLOAT4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42);
    planes[4].p = XMFLOAT4(m._13, m._23, m._33, m._43);
    planes[5].p = XMFLOAT4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43);

    for (int i = 0; i < 6; ++i)
        planes[i] = NormalizePlane(planes[i]);
}

void ComputeWorldAABB(const XMMATRIX& model, XMFLOAT3& outMin, XMFLOAT3& outMax)
{
    const XMFLOAT3 corners[8] =
    {
        {-0.5f,-0.5f,-0.5f}, { 0.5f,-0.5f,-0.5f},
        {-0.5f, 0.5f,-0.5f}, { 0.5f, 0.5f,-0.5f},
        {-0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f, 0.5f},
        {-0.5f, 0.5f, 0.5f}, { 0.5f, 0.5f, 0.5f}
    };

    XMFLOAT3 mn(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for (int i = 0; i < 8; ++i)
    {
        XMVECTOR p = XMVector3TransformCoord(XMLoadFloat3(&corners[i]), model);
        XMFLOAT3 wp;
        XMStoreFloat3(&wp, p);

        mn.x = min(mn.x, wp.x); mn.y = min(mn.y, wp.y); mn.z = min(mn.z, wp.z);
        mx.x = max(mx.x, wp.x); mx.y = max(mx.y, wp.y); mx.z = max(mx.z, wp.z);
    }

    outMin = mn;
    outMax = mx;
}

void RunGPUFrustumCulling(const Plane planes[6], const std::vector<InstanceDataGPU>& instanceData)
{
    if (!g_pCullCS || instanceData.empty())
        return;

    FrustumBufferCPU fr = {};
    for (int i = 0; i < 6; ++i)
        fr.frustum[i] = planes[i].p;
    g_pDeviceContext->UpdateSubresource(g_pFrustumBuffer, 0, nullptr, &fr, 0, 0);

    CullParamsCPU cp = {};
    cp.numShapes = XMUINT4((UINT)instanceData.size(), 0, 0, 0);

    for (UINT i = 0; i < (UINT)instanceData.size(); ++i)
    {
        XMMATRIX model = XMMatrixTranspose(XMLoadFloat4x4(&instanceData[i].model));

        XMFLOAT3 mn, mx;
        ComputeWorldAABB(model, mn, mx);

        cp.bbMin[i].v = XMFLOAT4(mn.x, mn.y, mn.z, 0.0f);
        cp.bbMax[i].v = XMFLOAT4(mx.x, mx.y, mx.z, 0.0f);
    }

    g_pDeviceContext->UpdateSubresource(g_pCullParamsBuffer, 0, nullptr, &cp, 0, 0);

    D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args = {};
    args.IndexCountPerInstance = 36;
    args.InstanceCount = 0;
    args.StartIndexLocation = 0;
    args.BaseVertexLocation = 0;
    args.StartInstanceLocation = 0;
    g_pDeviceContext->UpdateSubresource(g_pIndirectArgsSrc, 0, nullptr, &args, 0, 0);

    ID3D11Buffer* cbs[2] = { g_pFrustumBuffer, g_pCullParamsBuffer };
    g_pDeviceContext->CSSetConstantBuffers(0, 2, cbs);

    ID3D11UnorderedAccessView* uavs[2] = { g_pIndirectArgsUAV, g_pVisibleIdsUAV };
    g_pDeviceContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

    g_pDeviceContext->CSSetShader(g_pCullCS, nullptr, 0);

    UINT groupCount = DivUp((UINT)instanceData.size(), 64u);
    g_pDeviceContext->Dispatch(groupCount, 1, 1);

    ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
    g_pDeviceContext->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
    ID3D11Buffer* nullCBs[2] = { nullptr, nullptr };
    g_pDeviceContext->CSSetConstantBuffers(0, 2, nullCBs);
    g_pDeviceContext->CSSetShader(nullptr, nullptr, 0);

    g_pDeviceContext->CopyResource(g_pIndirectArgs, g_pIndirectArgsSrc);
}

void ReadGPUQueries()
{
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats = {};

    while (g_LastCompletedFrame < g_CurFrame)
    {
        HRESULT hr = g_pDeviceContext->GetData(
            g_pPipelineQueries[g_LastCompletedFrame % QUERY_COUNT],
            &stats,
            sizeof(stats),
            0);

        if (hr == S_OK)
        {
            g_GPUVisibleInstances = (UINT)(stats.IAPrimitives / 12);
            ++g_LastCompletedFrame;
        }
        else
        {
            break;
        }
    }
}

// Frame rendering
void RenderFrame()
{
    if (!g_pDeviceContext || !g_pBackBufferRTV || !g_pSwapChain || !g_pSceneColorRTV)
        return;

    double currentTime = (double)GetTickCount64() / 1000.0;
    double deltaTime = currentTime - g_LastTime;
    g_LastTime = currentTime;

    UpdateCamera(deltaTime);

    g_pDeviceContext->ClearState();

    float angle = (float)currentTime * 0.65f;

    float camX = g_CameraDist * sinf(g_CameraYaw) * cosf(g_CameraPitch);
    float camY = g_CameraDist * sinf(g_CameraPitch);
    float camZ = g_CameraDist * cosf(g_CameraYaw) * cosf(g_CameraPitch);

    XMVECTOR eye = XMVectorSet(camX, camY, camZ, 1.0f);
    XMVECTOR target = XMVectorSet(0, 0, 0, 1.0f);
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    XMMATRIX view = XMMatrixLookAtLH(eye, target, up);
    float aspect = (float)g_ClientWidth / (float)g_ClientHeight;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PI / 3.0f, aspect, 0.1f, 100.0f);
    XMMATRIX vp = XMMatrixMultiply(view, proj);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = g_pDeviceContext->Map(g_pViewProjBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        ViewProjBuffer* pVP = (ViewProjBuffer*)mapped.pData;
        pVP->vp = XMMatrixTranspose(vp);
        g_pDeviceContext->Unmap(g_pViewProjBuffer, 0);
    }

    SceneBuffer sceneData = {};
    sceneData.cameraPos = XMFLOAT4(camX, camY, camZ, 1.0f);
    sceneData.ambientColor = XMFLOAT4(0.22f, 0.22f, 0.24f, 1.0f);
    sceneData.lightCount = XMINT4(2, 0, 0, 0);

    sceneData.lights[0].position = XMFLOAT4(4.5f * cosf(angle), 2.2f, 4.5f * sinf(angle), 1.0f);
    sceneData.lights[0].color = XMFLOAT4(1.0f, 0.82f, 0.72f, 1.0f);

    sceneData.lights[1].position = XMFLOAT4(-4.5f, 1.2f + 0.8f * sinf(angle * 1.5f), -3.2f, 1.0f);
    sceneData.lights[1].color = XMFLOAT4(0.55f, 0.75f, 1.0f, 1.0f);

    g_pDeviceContext->UpdateSubresource(g_pSceneBuffer, 0, nullptr, &sceneData, 0, 0);

    Plane planes[6];
    ExtractFrustumPlanes(planes, vp);

    std::vector<InstanceDataGPU> instanceData(g_OpaqueCubes.size());
    for (size_t i = 0; i < g_OpaqueCubes.size(); ++i)
    {
        const CubeInstanceCPU& c = g_OpaqueCubes[i];

        float currentAngle = angle * c.rotSpeed;
        XMMATRIX model =
            XMMatrixScaling(c.scale, c.scale, c.scale) *
            XMMatrixRotationY(currentAngle) *
            XMMatrixTranslation(c.basePos.x, c.basePos.y, c.basePos.z);

        XMMATRIX normalM = XMMatrixTranspose(XMMatrixInverse(nullptr, model));

        InstanceDataGPU gpu = {};
        XMStoreFloat4x4(&gpu.model, XMMatrixTranspose(model));
        XMStoreFloat4x4(&gpu.normalMatrix, normalM);
        gpu.params = XMFLOAT4(32.0f, c.rotSpeed, (float)c.textureId, c.hasNormalMap ? 1.0f : 0.0f);
        gpu.posAngle = XMFLOAT4(c.basePos.x, c.basePos.y, c.basePos.z, currentAngle);
        instanceData[i] = gpu;
    }

    if (!instanceData.empty())
        g_pDeviceContext->UpdateSubresource(g_pInstanceBuffer, 0, nullptr, instanceData.data(), 0, 0);

    if (g_UseGPUCull)
        RunGPUFrustumCulling(planes, instanceData);

    ID3D11RenderTargetView* sceneViews[] = { g_pSceneColorRTV };
    g_pDeviceContext->OMSetRenderTargets(1, sceneViews, g_pDepthStencilView);

    const FLOAT clearColor[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
    g_pDeviceContext->ClearRenderTargetView(g_pSceneColorRTV, clearColor);
    g_pDeviceContext->ClearDepthStencilView(g_pDepthStencilView, D3D11_CLEAR_DEPTH, 1.0f, 0);

    D3D11_VIEWPORT viewport = { 0, 0, (FLOAT)g_ClientWidth, (FLOAT)g_ClientHeight, 0.0f, 1.0f };
    g_pDeviceContext->RSSetViewports(1, &viewport);

    ID3D11SamplerState* samplers0[] = { g_pSampler };

    UINT litStride = sizeof(LitVertex);
    UINT skyStride = sizeof(SkyboxVertex);
    UINT offset = 0;

    // OPAQUE INSTANCED CUBES
    g_pDeviceContext->OMSetDepthStencilState(g_pOpaqueDepthState, 0);
    g_pDeviceContext->RSSetState(g_pCullBackRS);

    g_pDeviceContext->VSSetShader(g_pVertexShader, nullptr, 0);
    g_pDeviceContext->PSSetShader(g_pPixelShader, nullptr, 0);
    g_pDeviceContext->IASetInputLayout(g_pInputLayout);

    ID3D11Buffer* vbCube[] = { g_pVertexBuffer };
    g_pDeviceContext->IASetVertexBuffers(0, 1, vbCube, &litStride, &offset);
    g_pDeviceContext->IASetIndexBuffer(g_pIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
    g_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11ShaderResourceView* cubeSRVs[] = { g_pTextureArrayView, g_pNormalTextureView };
    g_pDeviceContext->PSSetShaderResources(0, 2, cubeSRVs);
    g_pDeviceContext->PSSetSamplers(0, 1, samplers0);

    g_pDeviceContext->VSSetConstantBuffers(1, 1, &g_pViewProjBuffer);
    g_pDeviceContext->VSSetConstantBuffers(3, 1, &g_pInstanceBuffer);
    g_pDeviceContext->PSSetConstantBuffers(2, 1, &g_pSceneBuffer);
    g_pDeviceContext->PSSetConstantBuffers(3, 1, &g_pInstanceBuffer);

    if (g_UseGPUCull)
    {
        g_pDeviceContext->VSSetShaderResources(6, 1, &g_pVisibleIdsSRV);
        g_pDeviceContext->Begin(g_pPipelineQueries[g_CurFrame % QUERY_COUNT]);
        g_pDeviceContext->DrawIndexedInstancedIndirect(g_pIndirectArgs, 0);
        g_pDeviceContext->End(g_pPipelineQueries[g_CurFrame % QUERY_COUNT]);
        ++g_CurFrame;
    }
    else
    {
        ID3D11ShaderResourceView* nullVSBeforeUpdate[1] = { nullptr };
        g_pDeviceContext->VSSetShaderResources(6, 1, nullVSBeforeUpdate);

        std::vector<VisibleIdGPU> cpuVisible(g_OpaqueCubes.size());
        for (UINT i = 0; i < (UINT)g_OpaqueCubes.size(); ++i)
            cpuVisible[i].id = i;

        g_pDeviceContext->UpdateSubresource(g_pVisibleIdsGPU, 0, nullptr, cpuVisible.data(), 0, 0);
        g_pDeviceContext->VSSetShaderResources(6, 1, &g_pVisibleIdsSRV);
        g_pDeviceContext->DrawIndexedInstanced(36, (UINT)g_OpaqueCubes.size(), 0, 0, 0);
        g_GPUVisibleInstances = (UINT)g_OpaqueCubes.size();
    }

    {
        ID3D11ShaderResourceView* nullVS[1] = { nullptr };
        g_pDeviceContext->VSSetShaderResources(6, 1, nullVS);
    }

    // SKYBOX
    {
        XMMATRIX viewNoTranslate = view;
        viewNoTranslate.r[3] = XMVectorSet(0, 0, 0, 1);
        XMMATRIX vpSky = XMMatrixMultiply(viewNoTranslate, proj);

        D3D11_MAPPED_SUBRESOURCE mappedSky = {};
        hr = g_pDeviceContext->Map(g_pViewProjBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedSky);
        if (SUCCEEDED(hr))
        {
            ViewProjBuffer* pVP = (ViewProjBuffer*)mappedSky.pData;
            pVP->vp = XMMatrixTranspose(vpSky);
            g_pDeviceContext->Unmap(g_pViewProjBuffer, 0);
        }

        g_pDeviceContext->OMSetDepthStencilState(g_pSkyboxDepthState, 0);
        g_pDeviceContext->RSSetState(g_pCullNoneRS);

        g_pDeviceContext->VSSetShader(g_pSkyboxVS, nullptr, 0);
        g_pDeviceContext->PSSetShader(g_pSkyboxPS, nullptr, 0);
        g_pDeviceContext->IASetInputLayout(g_pSkyboxInputLayout);

        ID3D11Buffer* vbSky[] = { g_pSkyboxVertexBuffer };
        g_pDeviceContext->IASetVertexBuffers(0, 1, vbSky, &skyStride, &offset);
        g_pDeviceContext->IASetIndexBuffer(g_pSkyboxIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
        g_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D11ShaderResourceView* skySRV[] = { g_pCubemapView };
        g_pDeviceContext->PSSetShaderResources(5, 1, skySRV);
        g_pDeviceContext->PSSetSamplers(1, 1, samplers0);

        ID3D11Buffer* cbsSky[] = { nullptr, g_pViewProjBuffer };
        g_pDeviceContext->VSSetConstantBuffers(0, 2, cbsSky);

        g_pDeviceContext->DrawIndexed(36, 0, 0);

        D3D11_MAPPED_SUBRESOURCE mappedRestore = {};
        hr = g_pDeviceContext->Map(g_pViewProjBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedRestore);
        if (SUCCEEDED(hr))
        {
            ViewProjBuffer* pVP = (ViewProjBuffer*)mappedRestore.pData;
            pVP->vp = XMMatrixTranspose(vp);
            g_pDeviceContext->Unmap(g_pViewProjBuffer, 0);
        }
    }

    // TRANSPARENT OBJECTS
    std::vector<TransparentObject> transparentObjects;

    transparentObjects.push_back({
        XMMatrixScaling(1.2f, 1.2f, 0.08f) * XMMatrixTranslation(-2.8f, 0.0f, 1.2f),
        XMFLOAT4(1.0f, 0.2f, 0.2f, 0.45f),
        XMFLOAT3(-2.8f, 0.0f, 1.2f),
        0.0f
        });

    transparentObjects.push_back({
        XMMatrixScaling(1.2f, 1.2f, 0.08f) * XMMatrixRotationY(angle * 0.5f) * XMMatrixTranslation(0.6f, 0.2f, -2.3f),
        XMFLOAT4(0.2f, 0.8f, 1.0f, 0.45f),
        XMFLOAT3(0.6f, 0.2f, -2.3f),
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
            return a.distanceToCamera > b.distanceToCamera;
        });

    FLOAT blendFactor[4] = { 0, 0, 0, 0 };
    g_pDeviceContext->OMSetBlendState(g_pTransparentBlendState, blendFactor, 0xFFFFFFFF);
    g_pDeviceContext->OMSetDepthStencilState(g_pTransparentDepthState, 0);
    g_pDeviceContext->RSSetState(g_pCullBackRS);

    g_pDeviceContext->VSSetShader(g_pTransparentVS, nullptr, 0);
    g_pDeviceContext->PSSetShader(g_pTransparentPS, nullptr, 0);
    g_pDeviceContext->IASetInputLayout(g_pTransparentInputLayout);

    g_pDeviceContext->IASetVertexBuffers(0, 1, vbCube, &litStride, &offset);
    g_pDeviceContext->IASetIndexBuffer(g_pIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
    g_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (const auto& obj : transparentObjects)
    {
        TransparentBuffer tb = {};
        tb.model = XMMatrixTranspose(obj.model);
        tb.color = obj.color;
        g_pDeviceContext->UpdateSubresource(g_pTransparentBuffer, 0, nullptr, &tb, 0, 0);

        ID3D11Buffer* cbsTransparent[] = { g_pTransparentBuffer, g_pViewProjBuffer };
        g_pDeviceContext->VSSetConstantBuffers(0, 2, cbsTransparent);

        g_pDeviceContext->DrawIndexed(36, 0, 0);
    }

    g_pDeviceContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    {
        ID3D11ShaderResourceView* nullPS[6] = {};
        g_pDeviceContext->PSSetShaderResources(0, 6, nullPS);
    }

    PostProcessBuffer ppData = {};
    ppData.mode = XMINT4(g_PostEffectMode, 0, 0, 0);
    g_pDeviceContext->UpdateSubresource(g_pPostProcessBuffer, 0, nullptr, &ppData, 0, 0);

    ID3D11RenderTargetView* bbViews[] = { g_pBackBufferRTV };
    g_pDeviceContext->OMSetRenderTargets(1, bbViews, nullptr);
    g_pDeviceContext->ClearRenderTargetView(g_pBackBufferRTV, clearColor);

    g_pDeviceContext->OMSetDepthStencilState(nullptr, 0);
    g_pDeviceContext->RSSetState(nullptr);
    g_pDeviceContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    g_pDeviceContext->IASetInputLayout(nullptr);
    g_pDeviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    g_pDeviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    g_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    g_pDeviceContext->VSSetShader(g_pPostVS, nullptr, 0);
    g_pDeviceContext->PSSetShader(g_pPostPS, nullptr, 0);

    g_pDeviceContext->PSSetConstantBuffers(0, 1, &g_pPostProcessBuffer);
    g_pDeviceContext->PSSetSamplers(0, 1, &g_pSampler);
    g_pDeviceContext->PSSetShaderResources(0, 1, &g_pSceneColorSRV);

    g_pDeviceContext->Draw(6, 0);

    {
        ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
        g_pDeviceContext->PSSetShaderResources(0, 1, nullSRV);
    }

    if (g_UseGPUCull)
        ReadGPUQueries();

    wchar_t title[256];
    swprintf_s(title,
        L"Thu Hoai - GPU Frustum Culling | GPU visible instances: %u | Press G to toggle",
        g_GPUVisibleInstances);
    SetWindowTextW(g_hWnd, title);

    g_pSwapChain->Present(1, 0);
}

// Resize / cleanup
void OnResize(UINT newWidth, UINT newHeight)
{
    if (!g_pSwapChain || !g_pDevice || !g_pDeviceContext)
        return;

    g_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);

    SAFE_RELEASE(g_pBackBufferRTV);
    SAFE_RELEASE(g_pDepthStencilView);
    SAFE_RELEASE(g_pDepthStencilTexture);

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

    hr = g_pDevice->CreateTexture2D(&depthDesc, nullptr, &g_pDepthStencilTexture);
    if (FAILED(hr)) return;

    hr = g_pDevice->CreateDepthStencilView(g_pDepthStencilTexture, nullptr, &g_pDepthStencilView);
    if (FAILED(hr)) return;

    if (!CreatePostProcessResources(newWidth, newHeight))
        return;

    g_ClientWidth = newWidth;
    g_ClientHeight = newHeight;
}

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

    SAFE_RELEASE(g_pNormalTextureView);
    SAFE_RELEASE(g_pNormalTexture);

    SAFE_RELEASE(g_pTextureArrayView);
    SAFE_RELEASE(g_pTextureArray);

    SAFE_RELEASE(g_pCubemapView);
    SAFE_RELEASE(g_pCubemapTexture);

    SAFE_RELEASE(g_pSceneColorSRV);
    SAFE_RELEASE(g_pSceneColorRTV);
    SAFE_RELEASE(g_pSceneColorTex);

    SAFE_RELEASE(g_pViewProjBuffer);
    SAFE_RELEASE(g_pSceneBuffer);
    SAFE_RELEASE(g_pTransparentBuffer);
    SAFE_RELEASE(g_pPostProcessBuffer);
    SAFE_RELEASE(g_pInstanceBuffer);

    SAFE_RELEASE(g_pFrustumBuffer);
    SAFE_RELEASE(g_pCullParamsBuffer);
    SAFE_RELEASE(g_pVisibleIdsSRV);
    SAFE_RELEASE(g_pVisibleIdsUAV);
    SAFE_RELEASE(g_pVisibleIdsGPU);
    SAFE_RELEASE(g_pIndirectArgsUAV);
    SAFE_RELEASE(g_pIndirectArgsSrc);
    SAFE_RELEASE(g_pIndirectArgs);
    for (UINT i = 0; i < QUERY_COUNT; ++i)
        SAFE_RELEASE(g_pPipelineQueries[i]);

    SAFE_RELEASE(g_pInputLayout);
    SAFE_RELEASE(g_pVertexShader);
    SAFE_RELEASE(g_pPixelShader);
    SAFE_RELEASE(g_pCullCS);

    SAFE_RELEASE(g_pSkyboxInputLayout);
    SAFE_RELEASE(g_pSkyboxVS);
    SAFE_RELEASE(g_pSkyboxPS);

    SAFE_RELEASE(g_pTransparentInputLayout);
    SAFE_RELEASE(g_pTransparentVS);
    SAFE_RELEASE(g_pTransparentPS);

    SAFE_RELEASE(g_pPostVS);
    SAFE_RELEASE(g_pPostPS);

    SAFE_RELEASE(g_pIndexBuffer);
    SAFE_RELEASE(g_pVertexBuffer);
    SAFE_RELEASE(g_pSkyboxIndexBuffer);
    SAFE_RELEASE(g_pSkyboxVertexBuffer);

    SAFE_RELEASE(g_pBackBufferRTV);
    SAFE_RELEASE(g_pDepthStencilView);
    SAFE_RELEASE(g_pDepthStencilTexture);
    SAFE_RELEASE(g_pSwapChain);

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
