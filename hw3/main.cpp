#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")

using namespace DirectX;

#define SAFE_RELEASE(x) if(x){ x->Release(); x=nullptr; }

// Глобальные переменные
ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gContext = nullptr;
IDXGISwapChain* gSwapChain = nullptr;
ID3D11RenderTargetView* gRTV = nullptr;
ID3D11DepthStencilView* gDSV = nullptr;
ID3D11DepthStencilState* gDepthState = nullptr;
ID3D11RasterizerState* gRasterState = nullptr;

ID3D11Buffer* gVB = nullptr;
ID3D11Buffer* gIB = nullptr;

ID3D11Buffer* gModelBuffer = nullptr;
ID3D11Buffer* gVPBuffer = nullptr;

ID3D11VertexShader* gVS = nullptr;
ID3D11PixelShader* gPS = nullptr;
ID3D11InputLayout* gLayout = nullptr;

UINT gWidth = 1280;
UINT gHeight = 720;

float gAngle = 0.0f;

// Структуры
struct Vertex
{
    XMFLOAT3 pos;
    XMFLOAT4 color;
};

struct ModelBuffer
{
    XMMATRIX m;
};

struct VPBuffer
{
    XMMATRIX vp;
};

// Обработчик окна
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DESTROY)
        PostQuitMessage(0);

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// Инициализация DirectX
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

    D3D_FEATURE_LEVEL level;

    D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr, 0,
        D3D11_SDK_VERSION,
        &scd,
        &gSwapChain,
        &gDevice,
        &level,
        &gContext);

    // Создание render target
    ID3D11Texture2D* backBuffer = nullptr;
    gSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    gDevice->CreateRenderTargetView(backBuffer, nullptr, &gRTV);
    SAFE_RELEASE(backBuffer);

    // Создание depth buffer
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

    // Включение depth test
    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS;

    gDevice->CreateDepthStencilState(&dsDesc, &gDepthState);
    gContext->OMSetDepthStencilState(gDepthState, 0);

    // Отключаем culling 
    D3D11_RASTERIZER_DESC rsDesc{};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;

    gDevice->CreateRasterizerState(&rsDesc, &gRasterState);
    gContext->RSSetState(gRasterState);

    gContext->OMSetRenderTargets(1, &gRTV, gDSV);

    // Настройка viewport
    D3D11_VIEWPORT vp{};
    vp.Width = (float)gWidth;
    vp.Height = (float)gHeight;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    gContext->RSSetViewports(1, &vp);
}

// Создание куба


void CreateCube()
{
    float s = 0.4f;

    Vertex vertices[] =
    {
        {{-s,-s,-s},{1,0,0,1}},
        {{-s, s,-s},{0,1,0,1}},
        {{ s, s,-s},{0,0,1,1}},
        {{ s,-s,-s},{1,1,0,1}},
        {{-s,-s, s},{1,0,1,1}},
        {{-s, s, s},{0,1,1,1}},
        {{ s, s, s},{1,1,1,1}},
        {{ s,-s, s},{0,0,0,1}}
    };

    UINT indices[] =
    {
        0,1,2, 0,2,3,
        4,6,5, 4,7,6,
        4,5,1, 4,1,0,
        3,2,6, 3,6,7,
        1,5,6, 1,6,2,
        4,0,3, 4,3,7
    };

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(vertices);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = vertices;
    gDevice->CreateBuffer(&bd, &data, &gVB);

    bd.ByteWidth = sizeof(indices);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = indices;
    gDevice->CreateBuffer(&bd, &data, &gIB);
}

// Создание constant buffer
void CreateConstantBuffers()
{
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ModelBuffer);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    gDevice->CreateBuffer(&bd, nullptr, &gModelBuffer);

    bd.ByteWidth = sizeof(VPBuffer);
    gDevice->CreateBuffer(&bd, nullptr, &gVPBuffer);
}

// Обновление матриц
void UpdateMatrices()
{
    gAngle += 0.01f;

    // Вращение по двум осям
    XMMATRIX m = XMMatrixRotationY(gAngle) *
        XMMatrixRotationX(gAngle * 0.5f);

    ModelBuffer mb;
    mb.m = XMMatrixTranspose(m);
    gContext->UpdateSubresource(gModelBuffer, 0, nullptr, &mb, 0, 0);

    // Камера фиксированная
    XMVECTOR eye = XMVectorSet(0, 2, -5, 0);
    XMVECTOR at = XMVectorZero();
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    XMMATRIX v = XMMatrixLookAtLH(eye, at, up);
    XMMATRIX p = XMMatrixPerspectiveFovLH(
        XM_PIDIV4,
        (float)gWidth / gHeight,
        0.1f,
        100.0f);

    VPBuffer vp;
    vp.vp = XMMatrixTranspose(v * p);
    gContext->UpdateSubresource(gVPBuffer, 0, nullptr, &vp, 0, 0);
}

// Создание шейдеров
void CreateShaders()
{
    const char* vsCode =
        "cbuffer ModelBuffer:register(b0){matrix m;};"
        "cbuffer VPBuffer:register(b1){matrix vp;};"
        "struct VSInput{float3 pos:POSITION;float4 col:COLOR;};"
        "struct VSOutput{float4 pos:SV_Position;float4 col:COLOR;};"
        "VSOutput vs(VSInput i){"
        "VSOutput o;"
        "o.pos = mul(vp,mul(m,float4(i.pos,1)));"
        "o.col = i.col;"
        "return o;}";

    const char* psCode =
        "float4 ps(float4 pos:SV_Position,float4 col:COLOR):SV_Target{"
        "return col;}";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;

    D3DCompile(vsCode, strlen(vsCode), nullptr, nullptr, nullptr,
        "vs", "vs_5_0", 0, 0, &vsBlob, nullptr);

    D3DCompile(psCode, strlen(psCode), nullptr, nullptr, nullptr,
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
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,
         D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,
         D3D11_INPUT_PER_VERTEX_DATA,0}
    };

    gDevice->CreateInputLayout(layout, 2,
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        &gLayout);

    SAFE_RELEASE(vsBlob);
    SAFE_RELEASE(psBlob);
}

// Отрисовка кадра
void Render()
{
    float clear[4] = { 0.2f,0.3f,0.5f,1 };
    gContext->ClearRenderTargetView(gRTV, clear);
    gContext->ClearDepthStencilView(gDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;

    gContext->IASetVertexBuffers(0, 1, &gVB, &stride, &offset);
    gContext->IASetIndexBuffer(gIB, DXGI_FORMAT_R32_UINT, 0);
    gContext->IASetInputLayout(gLayout);
    gContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    gContext->VSSetShader(gVS, nullptr, 0);
    gContext->PSSetShader(gPS, nullptr, 0);

    gContext->VSSetConstantBuffers(0, 1, &gModelBuffer);
    gContext->VSSetConstantBuffers(1, 1, &gVPBuffer);

    gContext->DrawIndexed(36, 0, 0);

    gSwapChain->Present(1, 0);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"DX11Window";
    RegisterClass(&wc);

    HWND hWnd = CreateWindow(wc.lpszClassName,
        L"Thu Hoai - Cube 3D",
        WS_OVERLAPPEDWINDOW,
        100, 100, gWidth, gHeight,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hWnd, SW_SHOW);

    InitD3D(hWnd);
    CreateCube();
    CreateConstantBuffers();
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
