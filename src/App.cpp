#include "../headers/App.h"

App::App(HINSTANCE hInstance)
    : D3DApp(hInstance)
{}

App::~App()
{}

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

bool App::Initialize()
{
    if(!D3DApp::Initialize()) return false;
		
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    BuildModelGeometry("Models/Sponza/sponza.obj", "Models/Sponza/");

    BuildDescriptorHeaps();
    BuildConstantBuffers();

    mPassCB = std::make_unique<UploadBuffer<PassConstants>>(md3dDevice.Get(), 1, true);


    const UINT maxLights = 256;
    mLightBuffer = std::make_unique<UploadBuffer<Light>>(
        md3dDevice.Get(), maxLights, false);
    mLightBufferSize = maxLights;

    mPassCB = std::make_unique<UploadBuffer<PassConstants>>(md3dDevice.Get(), 1, true);

    // 1. Направленный свет
    Light dirLight;
    dirLight.Type = LIGHT_TYPE_DIRECTIONAL;
    dirLight.Strength = { 1.0f, 0.8f, 0.6f };
    dirLight.Direction = { 1.0f, -1.0f, 1.0f };
    mLights.push_back(dirLight);
    mMainLight = &mLights.back();


    // 2. Точечный свет
    Light pointLight;
    pointLight.Type = LIGHT_TYPE_POINT;
    pointLight.Strength = { 100.0f, 0.0f, 0.0f };
    pointLight.Position = { 710.0f, 100.0f, 0.0f };
    pointLight.Range = 400.0f;
    pointLight.FalloffStart = 0.0f;
    pointLight.FalloffEnd = pointLight.Range;
    mLights.push_back(pointLight);
    mPointLight = &mLights.back();

    // 3. Прожектор
    Light spotLight;
    spotLight.Type = LIGHT_TYPE_SPOT;
    spotLight.Strength = { 0.0f, 100.0f, 0.0f };
    spotLight.Position = { 0.0f, 200.0f, 0.0f };
    spotLight.Direction = { 0.0f, -1.0f, 0.0f };
    spotLight.Range = 300.0f;
    spotLight.SpotPower = 1.0f;
    spotLight.FalloffStart = spotLight.Range - 50.0f;
    spotLight.FalloffEnd = spotLight.Range;
    mLights.push_back(spotLight);
    mSpotLight = &mLights.back();

    Light pointLight2;
    pointLight2.Type = LIGHT_TYPE_POINT;
    pointLight2.Strength = { 0.0f, 0.0f, 150.0f };
    pointLight2.Position = { -700.0f, 500.0f, 400.0f };
    pointLight2.Range = 300.0f;
    pointLight2.FalloffStart = 0.0f;
    pointLight2.FalloffEnd = pointLight2.Range;
    mLights.push_back(pointLight2);
    mPointLight2 = &mLights.back();

    LoadAllTextures();

    DXGI_FORMAT gBufferFormats[3] = {
    DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R16G16B16A16_FLOAT
    };

    mGBuffer = std::make_unique<GBuffer>(md3dDevice.Get(), mClientWidth, mClientHeight);
    mGBuffer->BuildResources();

    mRenderSystem = std::make_unique<RenderingSystem>(
        md3dDevice.Get(),
        gBufferFormats,
        mDepthStencilFormat,
        m4xMsaaState,
        m4xMsaaQuality
    );
    mRenderSystem->Initialize();
    mRenderSystem->SetGBuffer(mGBuffer.get());

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

    mGBuffer->BuildDescriptors(hCpuSrv, hGpuSrv, hCpuRtv);

    for (UINT i = 0; i < SwapChainBufferCount; i++)
    {
        ComPtr<ID3D12Resource> backBuffer;
        ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
            mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
            i,
            mRtvDescriptorSize);

        md3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);
    }

    BuildSampler();

    mCamera = std::make_unique<Camera>();

    float x = mRadius * sinf(mPhi) * cosf(mTheta);
    float z = mRadius * sinf(mPhi) * sinf(mTheta);
    float y = mRadius * cosf(mPhi);
    mCamera->SetPosition(x, y, z);
    mCamera->SetLens(0.25f * MathHelper::Pi, AspectRatio(), 0.1f, 10000.0f);

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
    float dt = gt.DeltaTime();
    float speed = 400.0f * dt;

    DirectX::XMVECTOR look = mCamera->GetLook();
    DirectX::XMVECTOR right = mCamera->GetRight();
    DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT3 currentPos3f = mCamera->GetPosition3f();
    DirectX::XMVECTOR pos = DirectX::XMLoadFloat3(&currentPos3f);

    if (d3dUtil::IsKeyDown('W')) pos += look * speed;
    if (d3dUtil::IsKeyDown('S')) pos -= look * speed;
    if (d3dUtil::IsKeyDown('A')) pos -= right * speed;
    if (d3dUtil::IsKeyDown('D')) pos += right * speed;
    if (d3dUtil::IsKeyDown('E')) pos += up * speed;
    if (d3dUtil::IsKeyDown('Q')) pos -= up * speed;

    DirectX::XMFLOAT3 newPos;
    DirectX::XMStoreFloat3(&newPos, pos);
    mCamera->SetPosition(newPos);
    mCamera->UpdateViewMatrix();

    XMMATRIX view = mCamera->GetView();
    XMMATRIX proj = mCamera->GetProj();
    XMMATRIX world = XMLoadFloat4x4(&mWorld);
    XMMATRIX worldViewProj = world * view * proj;

	ObjectConstants objConstants;

    XMStoreFloat4x4(&objConstants.WorldViewProj, XMMatrixTranspose(worldViewProj));
    XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
    objConstants.gTime = gt.TotalTime();

    mObjectCB->CopyData(0, objConstants);

    PassConstants passConstants;
    XMStoreFloat4x4(&passConstants.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&passConstants.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&passConstants.ViewProj, XMMatrixTranspose(view * proj));
    passConstants.EyePosW = mCamera->GetPosition3f();
    passConstants.AmbientLight = mAmbientLight;
    passConstants.NumLights = (int)mLights.size();


    // Обновляем константный буфер
    mPassCB->CopyData(0, passConstants);

    // ============================================
    // КОПИРУЕМ ВСЕ ИСТОЧНИКИ В БУФЕР
    // ============================================
    UINT lightCount = (UINT)mLights.size();
    if (lightCount > mLightBufferSize)
    {
        // Если источников больше, чем буфер — расширяем
        mLightBuffer = std::make_unique<UploadBuffer<Light>>(
            md3dDevice.Get(), lightCount, false);
        mLightBufferSize = lightCount;
    }

    // Копируем все источники в буфер
    for (UINT i = 0; i < lightCount; ++i)
    {
        mLightBuffer->CopyData(i, mLights[i]);
    }
}

void App::Draw(const GameTimer& gt)
{
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mRenderSystem->BeginFrame(mCommandList.Get(), mScreenViewport, mScissorRect,
        mGBuffer.get(), DepthStencilView());

    ID3D12DescriptorHeap* heaps[] = { mCbvHeap.Get(), mSamplerHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(heaps), heaps);

    UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CD3DX12_GPU_DESCRIPTOR_HANDLE cbvHandle(mCbvHeap->GetGPUDescriptorHandleForHeapStart(), 0, descriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(0, cbvHandle);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(mCbvHeap->GetGPUDescriptorHandleForHeapStart(), 1, descriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);

    mCommandList->SetGraphicsRootDescriptorTable(2, mSamplerHeap->GetGPUDescriptorHandleForHeapStart());

    for (const auto& submeshInfo : mSubMeshInfos)
    {
        RenderItem item;
        item.Mesh = mGeo.get();
        item.SubmeshName = submeshInfo.name;
        item.CBIndex = 0;
        item.SRVIndex = (submeshInfo.textureIndex >= 0)
            ? kTextureSrvBase + submeshInfo.textureIndex
            : kTextureSrvBase;

        mRenderSystem->DrawItem(mCommandList.Get(), item, mCbvHeap.Get(), mSamplerHeap.Get(),true);
    }

    auto transition = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &transition);

    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::CornflowerBlue, 0, nullptr);

    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);

    mRenderSystem->EndFrame(
        mCommandList.Get(),
        mCbvHeap.Get(),
        mSamplerHeap.Get(),
        descriptorSize,
        mGBuffer.get(),
        mPassCB->Resource()->GetGPUVirtualAddress(),
        mLightBuffer->Resource()->GetGPUVirtualAddress(),
        (UINT)mLights.size());

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

void App::OnMouseMove(WPARAM btnState, int x, int y)
{
    float dx = static_cast<float>(x - mLastMousePos.x);
    float dy = static_cast<float>(y - mLastMousePos.y);

    float sensitivity = 0.005f;

    if ((btnState & MK_LBUTTON) != 0)
    {
        mCamera->RotateY(dx * sensitivity);
        mCamera->Pitch(dy * sensitivity);
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
    // Слот 0: CBV
    // Слот 1: SRV текстура-заглушка (опционально)
    // Слот 2..2+N: G-Buffer SRV (3 штуки)
    // Слот 5..5+mUniqueTextureCount: текстуры мешей
    UINT totalDescriptors = 5 + mUniqueTextureCount;

    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
    cbvHeapDesc.NumDescriptors = totalDescriptors;
    cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    cbvHeapDesc.NodeMask = 0;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&mCbvHeap)));

    // RTV heap: 2 swap chain + 3 GBuffer
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
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
    mSubMeshInfos.clear();

    // Словарь для уникальных материалов
    std::unordered_map<std::string, int> uniqueMaterials;
    int nextTextureIndex = 0;

    for (size_t shapeIdx = 0; shapeIdx < shapes.size(); ++shapeIdx)
    {
        const auto& shape = shapes[shapeIdx];

        int materialId = shape.mesh.material_ids.empty() ? -1 : shape.mesh.material_ids[0];

        SubMeshInfo submeshInfo;
        submeshInfo.name = shape.name.empty()
            ? "submesh_" + std::to_string(shapeIdx)
            : shape.name;

        if (materialId >= 0 && materialId < (int)materials.size())
        {
            submeshInfo.materialName = materials[materialId].name;

            // Получаем путь к диффузной текстуре
            if (!materials[materialId].diffuse_texname.empty())
            {
                std::string texPath = baseDir + materials[materialId].diffuse_texname;

                // Заменяем .tga на .dds
                size_t dotPos = texPath.rfind('.');
                if (dotPos != std::string::npos)
                    texPath = texPath.substr(0, dotPos) + ".dds";

                submeshInfo.texturePath = std::wstring(texPath.begin(), texPath.end());

                // Проверяем, загружали ли уже эту текстуру
                auto it = uniqueMaterials.find(submeshInfo.materialName);
                if (it == uniqueMaterials.end())
                {
                    uniqueMaterials[submeshInfo.materialName] = nextTextureIndex;
                    submeshInfo.textureIndex = nextTextureIndex;
                    nextTextureIndex++;
                }
                else
                {
                    submeshInfo.textureIndex = it->second;
                }
            }
            else
            {
                submeshInfo.texturePath = L"";
                submeshInfo.textureIndex = -1; // Нет текстуры
            }
        }
        else
        {
            submeshInfo.materialName = "default";
            submeshInfo.texturePath = L"";
            submeshInfo.textureIndex = -1;
        }

        UINT startVertex = (UINT)allVertices.size();
        UINT startIndex = (UINT)allIndices.size();

        for (const auto& index : shape.mesh.indices)
        {
            Vertex v;
            v.Pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };

            if (index.normal_index >= 0) {
                v.Normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2]
                };
            }
            else {
                v.Normal = { 0.0f, 1.0f, 0.0f }; // fallback, чтобы не было NaN/мусора
            }

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
        }

        for (size_t i = 0; i < shape.mesh.indices.size(); ++i) {
            allIndices.push_back(startVertex + (UINT)i);
        }

        SubmeshGeometry submesh;
        submesh.IndexCount = (UINT)shape.mesh.indices.size();
        submesh.StartIndexLocation = startIndex;
        submesh.BaseVertexLocation = 0;

        submeshInfo.indexCount = submesh.IndexCount;
        submeshInfo.startIndex = startIndex;

        mGeo->DrawArgs[submeshInfo.name] = submesh;
        mSubMeshInfos.push_back(submeshInfo);
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

    // Сохраняем информацию о количестве уникальных текстур
    mUniqueTextureCount = nextTextureIndex;
}

void App::LoadTextureForMaterial(const SubMeshInfo& submesh, int srvIndex)
{
    if (submesh.texturePath.empty())
    {
        // Создаём белую текстуру-заглушку для материалов без текстуры
        // Для простоты пока пропускаем
        return;
    }

    // Загружаем текстуру
    auto texture = std::make_unique<MeshTexture>();
    ThrowIfFailed(CreateDDSTextureFromFile12(
        md3dDevice.Get(),
        mCommandList.Get(),
        submesh.texturePath.c_str(),
        texture->Resource,
        texture->UploadHeap));

    // Сохраняем текстуру (нужно хранилище для всех текстур)
    // mTextures.push_back(std::move(texture));

    // Создаём SRV в куче
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = texture->Resource->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // SRV для текстуры (начинаем с индекса 2, так как 0 и 1 заняты)
    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(
        mCbvHeap->GetCPUDescriptorHandleForHeapStart(),
        2 + srvIndex,
        descriptorSize);

    md3dDevice->CreateShaderResourceView(texture->Resource.Get(), &srvDesc, hDescriptor);
}

void App::LoadAllTextures()
{
    UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Собираем уникальные текстуры (materialName -> textureIndex уже в mSubMeshInfos)
    std::unordered_map<std::string, bool> loaded;

    for (auto& submesh : mSubMeshInfos)
    {
        if (submesh.texturePath.empty() || submesh.textureIndex < 0) continue;
        if (loaded.count(submesh.materialName)) continue;
        loaded[submesh.materialName] = true;

        auto texture = std::make_unique<MeshTexture>();
        HRESULT hr = CreateDDSTextureFromFile12(
            md3dDevice.Get(), mCommandList.Get(),
            submesh.texturePath.c_str(),
            texture->Resource, texture->UploadHeap);

        if (FAILED(hr))
        {
            OutputDebugStringA(("Failed to load: " +
                std::string(submesh.texturePath.begin(), submesh.texturePath.end()) + "\n").c_str());
            submesh.textureIndex = -1;
            continue;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = texture->Resource->GetDesc().Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = -1; // все mip-уровни

        CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(
            mCbvHeap->GetCPUDescriptorHandleForHeapStart(),
            kTextureSrvBase + submesh.textureIndex,
            descriptorSize);

        md3dDevice->CreateShaderResourceView(texture->Resource.Get(), &srvDesc, hDescriptor);
        mTextures.push_back(std::move(texture));
    }
}