#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <vector>
#include <cmath>
#include <cstring>

#include "DDSTextureLoader.h"

#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")

using namespace DirectX;

#define SAFE_RELEASE(x) if(x){ (x)->Release(); (x)=nullptr; }

UINT gWidth = 1280, gHeight = 720;
float gAngle = 0.0f;
float gCameraAngle = 0.0f;

// DX objects
ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gContext = nullptr;
IDXGISwapChain* gSwapChain = nullptr;
ID3D11RenderTargetView* gRTV = nullptr;
ID3D11DepthStencilView* gDSV = nullptr;

ID3D11DepthStencilState* gDepthDefault = nullptr;
ID3D11DepthStencilState* gDepthSky = nullptr;

ID3D11RasterizerState* gRasterNoCull = nullptr;

// Cube
ID3D11Buffer* gCubeVB = nullptr;
ID3D11Buffer* gCubeIB = nullptr;

// Sky sphere
ID3D11Buffer* gSkyVB = nullptr;
ID3D11Buffer* gSkyIB = nullptr;
UINT gSkyIndexCount = 0;

// Constant buffers
ID3D11Buffer* gModelCB = nullptr;
ID3D11Buffer* gVPCB = nullptr;
ID3D11Buffer* gSkyCB = nullptr;

// Shaders/layouts
ID3D11VertexShader* gCubeVS = nullptr;
ID3D11PixelShader* gCubePS = nullptr;
ID3D11InputLayout* gCubeLayout = nullptr;

ID3D11VertexShader* gSkyVS = nullptr;
ID3D11PixelShader* gSkyPS = nullptr;
ID3D11InputLayout* gSkyLayout = nullptr;

// Textures + samplers
ID3D11ShaderResourceView* gCubeSRV = nullptr;
ID3D11ShaderResourceView* gSkySRV = nullptr;

ID3D11SamplerState* gSamplerWrap = nullptr;
ID3D11SamplerState* gSamplerClamp = nullptr;

// Structs
struct Vertex
{
    XMFLOAT3 pos;
    XMFLOAT2 uv;
};

struct SkyVertex
{
    XMFLOAT3 pos;
};

struct ModelCB { XMMATRIX m; };
struct VPCB { XMMATRIX vp; };
struct SkyCB { XMMATRIX vp; };

// Shader compile 
static ID3DBlob* Compile(const char* code, const char* entry, const char* profile)
{
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(code, (UINT)strlen(code), nullptr, nullptr, nullptr,
        entry, profile, 0, 0, &blob, &err);

    if (FAILED(hr))
    {
        if (err)
        {
            MessageBoxA(nullptr, (char*)err->GetBufferPointer(), "Shader compile error", MB_OK);
            err->Release();
        }
        ExitProcess(1);
    }
    if (err) err->Release();
    return blob;
}

// Window proc
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == 'A') gCameraAngle -= 0.05f;
        if (wParam == 'D') gCameraAngle += 0.05f;
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// Init D3D 
void InitD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = gWidth;
    scd.BufferDesc.Height = gHeight;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &gSwapChain, &gDevice, &level, &gContext);
    if (FAILED(hr)) ExitProcess(1);

    // RTV
    ID3D11Texture2D* backBuffer = nullptr;
    gSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    gDevice->CreateRenderTargetView(backBuffer, nullptr, &gRTV);
    SAFE_RELEASE(backBuffer);

    // Depth
    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = gWidth;
    depthDesc.Height = gHeight;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ID3D11Texture2D* depthTex = nullptr;
    gDevice->CreateTexture2D(&depthDesc, nullptr, &depthTex);
    gDevice->CreateDepthStencilView(depthTex, nullptr, &gDSV);
    SAFE_RELEASE(depthTex);

    gContext->OMSetRenderTargets(1, &gRTV, gDSV);

    D3D11_VIEWPORT vp{};
    vp.Width = (float)gWidth;
    vp.Height = (float)gHeight;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    gContext->RSSetViewports(1, &vp);

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    gDevice->CreateRasterizerState(&rs, &gRasterNoCull);
    gContext->RSSetState(gRasterNoCull);

    // Depth states
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    gDevice->CreateDepthStencilState(&ds, &gDepthDefault);

    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    gDevice->CreateDepthStencilState(&ds, &gDepthSky);
}

// Cube geometry
void CreateCube()
{
    float s = 0.5f;
    Vertex v[] =
    {
        // Front (-Z)
        {{-s,-s,-s},{0,1}}, {{-s, s,-s},{0,0}}, {{ s, s,-s},{1,0}}, {{ s,-s,-s},{1,1}},
        // Back (+Z)
        {{ s,-s, s},{0,1}}, {{ s, s, s},{0,0}}, {{-s, s, s},{1,0}}, {{-s,-s, s},{1,1}},
        // Left (-X)
        {{-s,-s, s},{0,1}}, {{-s, s, s},{0,0}}, {{-s, s,-s},{1,0}}, {{-s,-s,-s},{1,1}},
        // Right (+X)
        {{ s,-s,-s},{0,1}}, {{ s, s,-s},{0,0}}, {{ s, s, s},{1,0}}, {{ s,-s, s},{1,1}},
        // Top (+Y)
        {{-s, s,-s},{0,1}}, {{-s, s, s},{0,0}}, {{ s, s, s},{1,0}}, {{ s, s,-s},{1,1}},
        // Bottom (-Y)
        {{-s,-s, s},{0,1}}, {{-s,-s,-s},{0,0}}, {{ s,-s,-s},{1,0}}, {{ s,-s, s},{1,1}},
    };

    UINT idx[] =
    {
        0,1,2, 0,2,3,
        4,5,6, 4,6,7,
        8,9,10, 8,10,11,
        12,13,14, 12,14,15,
        16,17,18, 16,18,19,
        20,21,22, 20,22,23
    };

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = sizeof(v);

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = v;
    gDevice->CreateBuffer(&bd, &init, &gCubeVB);

    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = sizeof(idx);
    init.pSysMem = idx;
    gDevice->CreateBuffer(&bd, &init, &gCubeIB);
}

// Sky sphere geometry
void CreateSkySphere(int slice = 48, int stack = 24)
{
    std::vector<SkyVertex> verts;
    std::vector<UINT> inds;

    const float R = 50.0f;

    for (int i = 0; i <= stack; i++)
    {
        float phi = i * XM_PI / stack;
        for (int j = 0; j <= slice; j++)
        {
            float theta = j * XM_2PI / slice;
            float x = sinf(phi) * cosf(theta);
            float y = cosf(phi);
            float z = sinf(phi) * sinf(theta);
            verts.push_back({ XMFLOAT3(x * R, y * R, z * R) });
        }
    }

    for (int i = 0; i < stack; i++)
    {
        for (int j = 0; j < slice; j++)
        {
            UINT a = i * (slice + 1) + j;
            UINT b = a + (slice + 1);

            inds.push_back(a);     inds.push_back(b);     inds.push_back(a + 1);
            inds.push_back(a + 1); inds.push_back(b);     inds.push_back(b + 1);
        }
    }

    gSkyIndexCount = (UINT)inds.size();

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(verts.size() * sizeof(SkyVertex));

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = verts.data();
    gDevice->CreateBuffer(&bd, &init, &gSkyVB);

    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = (UINT)(inds.size() * sizeof(UINT));
    init.pSysMem = inds.data();
    gDevice->CreateBuffer(&bd, &init, &gSkyIB);
}

// Constant buffers 
void CreateConstantBuffers()
{
    // ModelCB (DEFAULT)
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(ModelCB);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.Usage = D3D11_USAGE_DEFAULT;
        gDevice->CreateBuffer(&bd, nullptr, &gModelCB);
    }

    // VPCB (DYNAMIC)
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(VPCB);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        gDevice->CreateBuffer(&bd, nullptr, &gVPCB);
    }

    // SkyCB (DYNAMIC)
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(SkyCB);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        gDevice->CreateBuffer(&bd, nullptr, &gSkyCB);
    }
}

// Textures 
void CreateTexturesAndSamplers()
{
    HRESULT hr = CreateDDSTextureFromFile(gDevice, L"cube.dds", nullptr, &gCubeSRV);
    if (FAILED(hr)) { MessageBoxW(nullptr, L"Failed to load cube.dds", L"Error", MB_OK); ExitProcess(1); }

    hr = CreateDDSTextureFromFile(gDevice, L"skybox.dds", nullptr, &gSkySRV);
    if (FAILED(hr)) { MessageBoxW(nullptr, L"Failed to load skybox.dds", L"Error", MB_OK); ExitProcess(1); }

    // Wrap sampler for cube
    {
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        s.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        gDevice->CreateSamplerState(&s, &gSamplerWrap);
    }

    // Clamp sampler for skybox
    {
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        gDevice->CreateSamplerState(&s, &gSamplerClamp);
    }
}

// Shaders 
void CreateShaders()
{
    const char* cubeVS =
        "cbuffer ModelCB:register(b0){matrix m;};"
        "cbuffer VPCB:register(b1){matrix vp;};"
        "struct VSIn{float3 pos:POSITION; float2 uv:TEXCOORD;};"
        "struct VSOut{float4 pos:SV_Position; float2 uv:TEXCOORD;};"
        "VSOut vs(VSIn i){"
        "  VSOut o;"
        "  o.pos = mul(vp, mul(m, float4(i.pos,1)));"
        "  o.uv = i.uv;"
        "  return o;"
        "}";

    const char* cubePS =
        "Texture2D tex:register(t0);"
        "SamplerState samp:register(s0);"
        "struct PSIn{float4 pos:SV_Position; float2 uv:TEXCOORD;};"
        "float4 ps(PSIn i):SV_Target{ return tex.Sample(samp, i.uv); }";

    ID3DBlob* vsb = Compile(cubeVS, "vs", "vs_5_0");
    ID3DBlob* psb = Compile(cubePS, "ps", "ps_5_0");

    gDevice->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &gCubeVS);
    gDevice->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &gCubePS);

    D3D11_INPUT_ELEMENT_DESC cubeLayout[] =
    {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,  D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,   0,12, D3D11_INPUT_PER_VERTEX_DATA,0},
    };
    gDevice->CreateInputLayout(cubeLayout, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &gCubeLayout);

    SAFE_RELEASE(vsb);
    SAFE_RELEASE(psb);

    const char* skyVS =
        "cbuffer SkyCB:register(b0){matrix vp;};"
        "struct VSIn{float3 pos:POSITION;};"
        "struct VSOut{float4 pos:SV_Position; float3 dir:TEXCOORD;};"
        "VSOut vs(VSIn i){"
        "  VSOut o;"
        "  o.pos = mul(vp, float4(i.pos,1));"
        "  o.dir = i.pos;"
        "  return o;"
        "}";

    const char* skyPS =
        "TextureCube sky:register(t0);"
        "SamplerState samp:register(s0);"
        "struct PSIn{float4 pos:SV_Position; float3 dir:TEXCOORD;};"
        "float4 ps(PSIn i):SV_Target{"
        "  return sky.Sample(samp, normalize(i.dir));"
        "}";

    vsb = Compile(skyVS, "vs", "vs_5_0");
    psb = Compile(skyPS, "ps", "ps_5_0");

    gDevice->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &gSkyVS);
    gDevice->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &gSkyPS);

    D3D11_INPUT_ELEMENT_DESC skyLayout[] =
    {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0, D3D11_INPUT_PER_VERTEX_DATA,0},
    };
    gDevice->CreateInputLayout(skyLayout, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &gSkyLayout);

    SAFE_RELEASE(vsb);
    SAFE_RELEASE(psb);
}

// Update
void UpdateMatrices()
{
    gAngle += 0.01f;

    // Cube model
    XMMATRIX m = XMMatrixRotationY(gAngle) * XMMatrixRotationX(gAngle * 0.5f);
    ModelCB mb{};
    mb.m = m;
    gContext->UpdateSubresource(gModelCB, 0, nullptr, &mb, 0, 0);

    // Camera
    float r = 6.0f;
    XMVECTOR eye = XMVectorSet(r * sinf(gCameraAngle), 2.0f, r * cosf(gCameraAngle), 1.0f);
    XMVECTOR at = XMVectorZero();
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    XMMATRIX v = XMMatrixLookAtLH(eye, at, up);
    XMMATRIX p = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)gWidth / (float)gHeight, 0.1f, 100.0f);

    // Cube VP
    VPCB vp{};
    vp.vp = v * p;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    gContext->Map(gVPCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, &vp, sizeof(VPCB));
    gContext->Unmap(gVPCB, 0);

    // Sky VP
    XMMATRIX vNoTrans = v;
    vNoTrans.r[3] = XMVectorSet(0, 0, 0, 1);

    SkyCB sky{};
    sky.vp = vNoTrans * p;

    gContext->Map(gSkyCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, &sky, sizeof(SkyCB));
    gContext->Unmap(gSkyCB, 0);
}

// Render 
void Render()
{
    float clear[4] = { 0,0,0,1 };
    gContext->ClearRenderTargetView(gRTV, clear);
    gContext->ClearDepthStencilView(gDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

    {
        gContext->OMSetDepthStencilState(gDepthSky, 0);

        UINT stride = sizeof(SkyVertex);
        UINT offset = 0;
        gContext->IASetVertexBuffers(0, 1, &gSkyVB, &stride, &offset);
        gContext->IASetIndexBuffer(gSkyIB, DXGI_FORMAT_R32_UINT, 0);
        gContext->IASetInputLayout(gSkyLayout);
        gContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        gContext->VSSetShader(gSkyVS, nullptr, 0);
        gContext->PSSetShader(gSkyPS, nullptr, 0);

        gContext->VSSetConstantBuffers(0, 1, &gSkyCB);

        gContext->PSSetShaderResources(0, 1, &gSkySRV);
        gContext->PSSetSamplers(0, 1, &gSamplerClamp);

        gContext->DrawIndexed(gSkyIndexCount, 0, 0);

        ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
        gContext->PSSetShaderResources(0, 1, nullSRV);
    }

    // Cube 
    {
        gContext->OMSetDepthStencilState(gDepthDefault, 0);

        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        gContext->IASetVertexBuffers(0, 1, &gCubeVB, &stride, &offset);
        gContext->IASetIndexBuffer(gCubeIB, DXGI_FORMAT_R32_UINT, 0);
        gContext->IASetInputLayout(gCubeLayout);
        gContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        gContext->VSSetShader(gCubeVS, nullptr, 0);
        gContext->PSSetShader(gCubePS, nullptr, 0);

        gContext->VSSetConstantBuffers(0, 1, &gModelCB);
        gContext->VSSetConstantBuffers(1, 1, &gVPCB);

        gContext->PSSetShaderResources(0, 1, &gCubeSRV);
        gContext->PSSetSamplers(0, 1, &gSamplerWrap);

        gContext->DrawIndexed(36, 0, 0);

        ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
        gContext->PSSetShaderResources(0, 1, nullSRV);
    }

    gSwapChain->Present(1, 0);
}

// WinMain
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"DX11Window";
    RegisterClass(&wc);

    HWND hWnd = CreateWindow(
        wc.lpszClassName,
        L"Thu Hoai_Cube + Skybox",
        WS_OVERLAPPEDWINDOW,
        100, 100, gWidth, gHeight,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hWnd, SW_SHOW);

    InitD3D(hWnd);
    CreateCube();
    CreateSkySphere();
    CreateConstantBuffers();
    CreateTexturesAndSamplers();
    CreateShaders();

    MSG msg{};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            UpdateMatrices();
            Render();
        }
    }

    return 0;
}