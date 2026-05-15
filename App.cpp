#include "../DirectX12Lab/framework/d3dApp.h"
#include "../DirectX12Lab/framework/MathHelper.h"
#include "../DirectX12Lab/framework/UploadBuffer.h"
#include "../DirectX12Lab/framework/DDSTextureLoader.h" 
#include "../DirectX12Lab/framework/RenderingSystem.h" 
#include "../DirectX12Lab/framework/GBuffer.h"
#include "../DirectX12Lab/headers/Camera.h"

#define NOMINMAX
#include <windows.h>
#include "../DirectX12Lab/framework/tiny_obj_loader.h" 

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

struct MeshTexture
{
    std::string Name;
    std::wstring Filename;
    ComPtr<ID3D12Resource> Resource = nullptr;
    ComPtr<ID3D12Resource> UploadHeap = nullptr;
};

struct SubMeshInfo {
    std::string name;          // имя shape из .obj
    UINT indexCount;           // сколько индексов у этого меша
    UINT startIndex;           // с какого места в общем буфере начинается
    std::wstring texturePath;  // путь к его текстуре
};

struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT2 TexCoord;
};

struct ObjectConstants
{
    XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
    float gTime = 0.0f;
    XMFLOAT3 padding;
};

class App : public D3DApp
{
public:
	App(HINSTANCE hInstance);
    App(const App& rhs) = delete;
    App& operator=(const App& rhs) = delete;
	~App();

	virtual bool Initialize()override;

private:
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void BuildDescriptorHeaps();
	void BuildConstantBuffers();

    void BuildTextureHeap();
    void LoadTexture(std::wstring const texturePath);
    void CreateTextureSRV();
    void BuildSampler();

    void BuildModelGeometry(std::string modelPath, std::string baseDir);

private:
    std::unique_ptr<Camera> mCamera;
    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12DescriptorHeap> mCbvHeap = nullptr;

    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;

	std::unique_ptr<MeshGeometry> mGeo = nullptr;

    ComPtr<ID3DBlob> mvsByteCode = nullptr;
    ComPtr<ID3DBlob> mpsByteCode = nullptr;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    ComPtr<ID3D12PipelineState> mPSO = nullptr;

    XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.5f*XM_PI;
    float mPhi = XM_PIDIV4;
    float mRadius = 5.0f;

    POINT mLastMousePos;
    std::vector<SubMeshInfo> mSubMeshInfos;
    std::unique_ptr<MeshTexture> mTexture;
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;
    ComPtr<ID3D12DescriptorHeap> mSamplerHeap;
    std::unique_ptr<RenderingSystem> mRenderSystem;
    std::unique_ptr<GBuffer> mGBuffer;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
				   PSTR cmdLine, int showCmd)
{
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
#endif

    try
    {
        App theApp(hInstance);
        if(!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch(DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

App::App(HINSTANCE hInstance)
: D3DApp(hInstance) 
{
}

App::~App()
{
}

void App::BuildTextureHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvHeap)));
}

void App::BuildSampler()
{
    D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
    samplerHeapDesc.NumDescriptors = 1;
    samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&mSamplerHeap)));

    D3D12_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    md3dDevice->CreateSampler(&samplerDesc, mSamplerHeap->GetCPUDescriptorHandleForHeapStart());
}

void App::LoadTexture(std::wstring const texturePath)
{
    mTexture = std::make_unique<MeshTexture>();

    ThrowIfFailed(CreateDDSTextureFromFile12(
        md3dDevice.Get(),
        mCommandList.Get(),
        texturePath.c_str(),
        mTexture->Resource,
        mTexture->UploadHeap));
}

void App::CreateTextureSRV()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = mTexture->Resource->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(
        mCbvHeap->GetCPUDescriptorHandleForHeapStart(),
        1,
        descriptorSize);

    md3dDevice->CreateShaderResourceView(mTexture->Resource.Get(), &srvDesc, hDescriptor);
}

bool App::Initialize()
{
    if(!D3DApp::Initialize())
		return false;
		
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));
    BuildDescriptorHeaps();

    DXGI_FORMAT gBufferFormats[3] = {
    DXGI_FORMAT_R8G8B8A8_UNORM,      // Альбедо (цвет)
    DXGI_FORMAT_R16G16B16A16_FLOAT,  // Нормали
    DXGI_FORMAT_R16G16B16A16_FLOAT   // Мировая позиция
    };

    mRenderSystem = std::make_unique<RenderingSystem>(
        md3dDevice.Get(),
        gBufferFormats,
        mDepthStencilFormat,
        m4xMsaaState,
        m4xMsaaQuality
    );
    mRenderSystem->Initialize();
    mGBuffer = std::make_unique<GBuffer>(md3dDevice.Get(), mClientWidth, mClientHeight);
    mGBuffer->BuildResources();

    UINT cbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT rtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    int gBufferSrvOffset = 2;
    int gBufferRtvOffset = 2;
    CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv(
        mCbvHeap->GetCPUDescriptorHandleForHeapStart(),
        gBufferSrvOffset,
        cbvSrvDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv(
        mCbvHeap->GetGPUDescriptorHandleForHeapStart(),
        gBufferSrvOffset,
        cbvSrvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv(
        mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
        gBufferRtvOffset,
        rtvDescriptorSize);

    //mGBuffer->BuildDescriptors(hCpuSrv, hGpuSrv, hCpuRtv);

    for (UINT i = 0; i < SwapChainBufferCount; i++)
    {
        ComPtr<ID3D12Resource> backBuffer;
        ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

        // Получаем хендл для 0 и 1 слота
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
            mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
            i,
            mRtvDescriptorSize);

        // Создаем View (теперь в новой куче есть данные для слотов 0 и 1)
        md3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);
    }

	BuildConstantBuffers();
    BuildModelGeometry("Models/Sponza/sponza.obj", "Models/");

    BuildTextureHeap();
    LoadTexture(L"Models/Sponza/textures/lion.dds");
    CreateTextureSRV();
    BuildSampler();


    mCamera = std::make_unique<Camera>();

    // Начальная позиция (орбитальная)
    float x = mRadius * sinf(mPhi) * cosf(mTheta);
    float z = mRadius * sinf(mPhi) * sinf(mTheta);
    float y = mRadius * cosf(mPhi);
    mCamera->SetPosition(x, y, z);
    mCamera->SetLens(0.25f * MathHelper::Pi, AspectRatio(), 0.1f, 10000.0f);
    mCamera->LookAt(XMFLOAT3(x, y, z), XMFLOAT3(0, 0, 0), XMFLOAT3(0, 1, 0));

    // Execute the initialization commands.
    ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Wait until initialization is complete.
    FlushCommandQueue();

	return true;
}

void App::OnResize()
{
	D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 10000.0f);
    XMStoreFloat4x4(&mProj, P);

    if (mCamera)
    {
        mCamera->SetLens(0.25f * MathHelper::Pi, AspectRatio(), 0.1f, 10000.0f);
    }
}

void App::Update(const GameTimer& gt)
{
    if (!mCamera) return;

    // 2. Обновляем матрицу вида
    mCamera->UpdateViewMatrix();

    // 3. Получаем матрицы для рендеринга

    XMMATRIX view = mCamera->GetView();
    XMMATRIX proj = mCamera->GetProj();


    XMMATRIX world = XMLoadFloat4x4(&mWorld);
    XMMATRIX worldViewProj = world * view * proj;

	// Update the constant buffer with the latest worldViewProj matrix.
	ObjectConstants objConstants;

    XMStoreFloat4x4(&objConstants.WorldViewProj, XMMatrixTranspose(worldViewProj));
    objConstants.gTime = gt.TotalTime();

    mObjectCB->CopyData(0, objConstants);
}

void App::Draw(const GameTimer& gt)
{
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr)); // ← убрал mPSO.Get()

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    auto transition = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &transition);

    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::CornflowerBlue, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    auto cbbv = CurrentBackBufferView();
    auto dsv = DepthStencilView();
    mCommandList->OMSetRenderTargets(1, &cbbv, true, &dsv);

    mRenderSystem->BeginFrame(mCommandList.Get(), mScreenViewport, mScissorRect);

    // ← ИСПРАВЛЕНО: рисуем ВСЕ submesh'и
    for (auto& pair : mGeo->DrawArgs)
    {
        RenderItem item;
        item.Mesh = mGeo.get();
        item.SubmeshName = pair.first;  // используем реальное имя из DrawArgs
        item.CBIndex = 0;
        item.SRVIndex = 1;  // пока все используют одну текстуру

        mRenderSystem->DrawItem(mCommandList.Get(), item, mCbvHeap.Get(), mSamplerHeap.Get());
    }

    transition = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &transition);

    ThrowIfFailed(mCommandList->Close());

    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    FlushCommandQueue();
}

void App::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;

    SetCapture(mhMainWnd);
}

void App::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void App::OnMouseMove(WPARAM btnState, int x, int y)
{
    float dx = static_cast<float>(x - mLastMousePos.x);
    float dy = static_cast<float>(y - mLastMousePos.y);

    float sensitivity = 0.005f;

    if ((btnState & MK_LBUTTON) != 0)
    {
        mCamera->RotateY(-dx * sensitivity);
        mCamera->Pitch(-dy * sensitivity);
    }

    else if ((btnState & MK_RBUTTON) != 0)
    {
        float zoomSpeed = 5.0f;
        mCamera->Walk(dx * zoomSpeed);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void App::BuildDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
    cbvHeapDesc.NumDescriptors = 10;
    cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	cbvHeapDesc.NodeMask = 0;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&cbvHeapDesc,
        IID_PPV_ARGS(&mCbvHeap)));

    // 2. Куча для RTV (SwapChain + G-буфер)
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    // Нужно: 2 (SwapChain) + 3 (G-буфер) = 5
    rtvHeapDesc.NumDescriptors = 5;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));
}

void App::BuildConstantBuffers()
{
	mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(md3dDevice.Get(), 1, true);

	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

	D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mObjectCB->Resource()->GetGPUVirtualAddress();
    // Offset to the ith object constant buffer in the buffer.
    int boxCBufIndex = 0;
	cbAddress += boxCBufIndex*objCBByteSize;

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
	cbvDesc.BufferLocation = cbAddress;
	cbvDesc.SizeInBytes = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

    md3dDevice->CreateConstantBufferView(&cbvDesc, mCbvHeap->GetCPUDescriptorHandleForHeapStart());
}

void App::BuildModelGeometry(std::string modelPath, std::string baseDir)
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
        modelPath.c_str(), baseDir.c_str());

    if (!ret) {
        OutputDebugStringA(err.c_str());
        return;
    }

    mGeo = std::make_unique<MeshGeometry>();
    mGeo->Name = "Sponza";

    std::vector<Vertex> allVertices;
    std::vector<std::uint32_t> allIndices;

    for (size_t shapeIdx = 0; shapeIdx < shapes.size(); ++shapeIdx)
    {
        const auto& shape = shapes[shapeIdx];

        UINT startVertex = (UINT)allVertices.size();  // <-- Запоминаем начало ВЕРШИН
        UINT startIndex = (UINT)allIndices.size();     // <-- Запоминаем начало ИНДЕКСОВ

        for (const auto& index : shape.mesh.indices)
        {
            Vertex v;
            v.Pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };

            if (index.texcoord_index >= 0) {
                v.TexCoord = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
                };
            }
            else {
                v.TexCoord = { 0.0f, 0.0f };
            }

            allVertices.push_back(v);
            allIndices.push_back((UINT)(startVertex + allIndices.size() - startIndex));
            // Индексы должны ссылаться на правильные вершины
        }

        SubmeshGeometry submesh;
        submesh.IndexCount = (UINT)shape.mesh.indices.size();
        submesh.StartIndexLocation = startIndex;
        submesh.BaseVertexLocation = 0;  // Можно 0, если индексы правильные

        std::string submeshName = shape.name.empty()
            ? "submesh_" + std::to_string(shapeIdx)
            : shape.name;

        mGeo->DrawArgs[submeshName] = submesh;
    }

    // Создаём GPU буферы
    const UINT vbByteSize = (UINT)allVertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)allIndices.size() * sizeof(std::uint32_t);

    mGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(
        md3dDevice.Get(), mCommandList.Get(),
        allVertices.data(), vbByteSize, mGeo->VertexBufferUploader);

    mGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(
        md3dDevice.Get(), mCommandList.Get(),
        allIndices.data(), ibByteSize, mGeo->IndexBufferUploader);

    mGeo->VertexByteStride = sizeof(Vertex);
    mGeo->VertexBufferByteSize = vbByteSize;
    mGeo->IndexFormat = DXGI_FORMAT_R32_UINT;
    mGeo->IndexBufferByteSize = ibByteSize;
}