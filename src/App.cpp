#include "../headers/App.h"

#include <filesystem>
#include <cfloat>
#include <stdexcept>

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
    mTessCB = std::make_unique<UploadBuffer<TessellationConstants>>(md3dDevice.Get(), 1, true);

    const UINT maxLights = 256;
    mLightBuffer = std::make_unique<UploadBuffer<Light>>(
        md3dDevice.Get(), maxLights, false);
    mLightBufferSize = maxLights;

    mPassCB = std::make_unique<UploadBuffer<PassConstants>>(md3dDevice.Get(), 1, true);

    // 1. Направленный свет
    Light dirLight;
    dirLight.Type = LIGHT_TYPE_DIRECTIONAL;
    dirLight.Strength = { 2.0f, 2.0f, 2.0f };
    dirLight.Direction = { 2.0f, -1.0f, 0.0f };
    mLights.push_back(dirLight);
    mMainLight = &mLights.back();

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

    mRadius = mSceneRadius * 2.0f;
    if (mRadius < 5.0f)
    {
        mRadius = 5.0f;
    }
    const float x = mSceneCenter.x + mRadius * sinf(mPhi) * cosf(mTheta);
    const float z = mSceneCenter.z + mRadius * sinf(mPhi) * sinf(mTheta);
    const float y = mSceneCenter.y + mRadius * cosf(mPhi);
    mCamera->SetPosition(x, y, z);
    const XMFLOAT3 cameraPosition = mCamera->GetPosition3f();
    mCamera->LookAt(XMLoadFloat3(&cameraPosition), XMLoadFloat3(&mSceneCenter),
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    mCamera->SetLens(0.25f * MathHelper::Pi, AspectRatio(), 0.1f, 10000.0f);

    // Execute the initialization commands.
    ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Wait until initialization is complete.
    FlushCommandQueue();
    mTextureUploadKeepAlive.clear();
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

    UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Each submesh owns a material SRV, so models may freely mix texture sizes.
    for (const auto& submeshInfo : mSubMeshInfos)
    {
        if (mEnableTessellation && submeshInfo.name.find("Well") != std::string::npos)
        {
            continue;
        }

        RenderItem item;
        item.Mesh = mGeo.get();
        item.SubmeshName = submeshInfo.name;
        item.CBIndex = 0;
        item.SRVIndex = kTextureSrvBase + submeshInfo.textureIndex;

        mRenderSystem->DrawItem(mCommandList.Get(), item, mCbvHeap.Get(),
            mSamplerHeap.Get(), true);
    }

    // ============================================
    // 2. ТЕССЕЛЯЦИЯ КОЛОДЦА
    // ============================================
    if (mEnableTessellation && mRenderSystem->GetTessellationPSO() && mTessCB)
    {
        // Вычисляем адаптивный фактор тесселяции
        XMFLOAT3 wellCenter = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 eyePos = mCamera->GetPosition3f();

        float distance = sqrt(
            pow(eyePos.x - wellCenter.x, 2) +
            pow(eyePos.y - wellCenter.y, 2) +
            pow(eyePos.z - wellCenter.z, 2));

        float minTess = 0.0f;
        float maxTess = 64.0f;
        float minDist = 10.0f;
        float maxDist = 500.0f;

        float tessFactor = maxTess - (distance - minDist) / (maxDist - minDist) * (maxTess - minTess);
        tessFactor = max(minTess, min(maxTess, tessFactor));

        // Обновляем константы тесселяции
        XMMATRIX view = mCamera->GetView();
        XMMATRIX proj = mCamera->GetProj();
        XMMATRIX world = XMLoadFloat4x4(&mWorld);
        XMMATRIX worldViewProj = world * view * proj;
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

        TessellationConstants tessConstants;
        XMStoreFloat4x4(&tessConstants.WorldViewProj, XMMatrixTranspose(worldViewProj));
        XMStoreFloat4x4(&tessConstants.World, XMMatrixTranspose(world));
        XMStoreFloat4x4(&tessConstants.WorldInvTranspose, XMMatrixTranspose(worldInvTranspose));
        tessConstants.EyePosW = mCamera->GetPosition3f();
        tessConstants.TessellationFactor = tessFactor;
        tessConstants.DisplacementScale = 5.0f;

        mTessCB->CopyData(0, tessConstants);

        // Привязываем PSO тесселяции
        mCommandList->SetPipelineState(mRenderSystem->GetTessellationPSO());
        mCommandList->SetGraphicsRootSignature(mRenderSystem->GetTessellationRootSig());

        // Константы (слот 0)
        mCommandList->SetGraphicsRootConstantBufferView(0,
            mTessCB->Resource()->GetGPUVirtualAddress());

        // ============================================
        // Слот 1: TEXTURE2DARRAY (один SRV)
        // ============================================
        CD3DX12_GPU_DESCRIPTOR_HANDLE texHandle(
            mCbvHeap->GetGPUDescriptorHandleForHeapStart(),
            kTextureSrvBase,  // ← Один SRV для всего массива
            descriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(1, texHandle);

        // Слот 2: Семплер
        mCommandList->SetGraphicsRootDescriptorTable(2,
            mSamplerHeap->GetGPUDescriptorHandleForHeapStart());

        // Геометрия
        auto vbv = mGeo->VertexBufferView();
        auto ibv = mGeo->IndexBufferView();
        mCommandList->IASetVertexBuffers(0, 1, &vbv);
        mCommandList->IASetIndexBuffer(&ibv);
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

        // Рисуем колодец
        for (const auto& submeshInfo : mSubMeshInfos)
        {
            if (submeshInfo.name.find("Well") == std::string::npos) continue;

            auto& submesh = mGeo->DrawArgs[submeshInfo.name];
            mCommandList->DrawIndexedInstanced(
                submesh.IndexCount,
                1,
                submesh.StartIndexLocation,
                submesh.BaseVertexLocation,
                0);
        }
    }

    // ============================================
    // LIGHTING PASS
    // ============================================
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
    const UINT textureDescriptorCount = mUniqueTextureCount > 0 ? mUniqueTextureCount : 1;
    const UINT totalDescriptors = kTextureSrvBase + textureDescriptorCount;

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
    std::string warning;
    std::string error;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warning, &error,
        modelPath.c_str(), baseDir.c_str(), true))
    {
        throw std::runtime_error("Unable to load OBJ '" + modelPath + "': " + error);
    }

    if (!warning.empty())
    {
        OutputDebugStringA(warning.c_str());
    }

    XMFLOAT3 minimum = { FLT_MAX, FLT_MAX, FLT_MAX };
    XMFLOAT3 maximum = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    const auto minimumOf = [](float left, float right) { return left < right ? left : right; };
    const auto maximumOf = [](float left, float right) { return left > right ? left : right; };
    for (size_t index = 0; index < attrib.vertices.size(); index += 3)
    {
        minimum.x = minimumOf(minimum.x, attrib.vertices[index]);
        minimum.y = minimumOf(minimum.y, attrib.vertices[index + 1]);
        minimum.z = minimumOf(minimum.z, attrib.vertices[index + 2]);
        maximum.x = maximumOf(maximum.x, attrib.vertices[index]);
        maximum.y = maximumOf(maximum.y, attrib.vertices[index + 1]);
        maximum.z = maximumOf(maximum.z, attrib.vertices[index + 2]);
    }
    mSceneCenter = {
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f };
    mSceneRadius = XMVectorGetX(XMVector3Length(XMLoadFloat3(&maximum) - XMLoadFloat3(&mSceneCenter)));

    auto texturePathFor = [&baseDir](const std::string& sourcePath)
    {
        if (sourcePath.empty())
        {
            return std::wstring();
        }

        std::filesystem::path path = std::filesystem::path(baseDir) / sourcePath;
        path.replace_extension(".dds");
        return path.wstring();
    };

    std::unordered_map<std::wstring, int> textureIndices;
    mSubMeshTextures.clear();
    std::vector<int> materialTextureIndices(materials.size(), 0);

    for (size_t materialIndex = 0; materialIndex < materials.size(); ++materialIndex)
    {
        const std::wstring texturePath = texturePathFor(materials[materialIndex].diffuse_texname);
        if (texturePath.empty())
        {
            continue;
        }

        const auto [iterator, inserted] = textureIndices.emplace(
            texturePath, static_cast<int>(mSubMeshTextures.size()));
        if (inserted)
        {
            SubMeshTextures textures;
            textures.albedoPath = texturePath;
            textures.arrayIndex = iterator->second;
            mSubMeshTextures.push_back(std::move(textures));
        }
        materialTextureIndices[materialIndex] = iterator->second;
    }

    if (mSubMeshTextures.empty())
    {
        throw std::runtime_error("OBJ does not contain a diffuse texture that can be loaded as DDS.");
    }

    mGeo = std::make_unique<MeshGeometry>();
    mGeo->Name = "Model";
    mSubMeshInfos.clear();

    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    auto appendSubmesh = [&](const std::string& name, int materialId,
        const std::vector<tinyobj::index_t>& faceIndices)
    {
        if (faceIndices.empty())
        {
            return;
        }

        const UINT startVertex = static_cast<UINT>(vertices.size());
        const UINT startIndex = static_cast<UINT>(indices.size());
        const int textureIndex = materialId >= 0 && materialId < static_cast<int>(materialTextureIndices.size())
            ? materialTextureIndices[materialId] : 0;

        for (const tinyobj::index_t& index : faceIndices)
        {
            Vertex vertex = {};
            if (index.vertex_index >= 0)
            {
                vertex.Pos = {
                    attrib.vertices[3 * index.vertex_index],
                    attrib.vertices[3 * index.vertex_index + 1],
                    attrib.vertices[3 * index.vertex_index + 2] };
            }
            if (index.normal_index >= 0)
            {
                vertex.Normal = {
                    attrib.normals[3 * index.normal_index],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2] };
            }
            else
            {
                vertex.Normal = { 0.0f, 1.0f, 0.0f };
            }
            if (index.texcoord_index >= 0)
            {
                vertex.TexCoord = {
                    attrib.texcoords[2 * index.texcoord_index],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1] };
            }
            vertex.Tangent = { 1.0f, 0.0f, 0.0f };
            vertex.TexIndex = static_cast<UINT>(textureIndex);
            vertices.push_back(vertex);
            indices.push_back(static_cast<UINT>(vertices.size() - 1));
        }

        const std::string uniqueName = name + "_" + std::to_string(mSubMeshInfos.size());
        mGeo->DrawArgs[uniqueName] = { static_cast<UINT>(faceIndices.size()), startIndex, 0 };

        SubMeshInfo info = {};
        info.name = uniqueName;
        info.indexCount = static_cast<UINT>(faceIndices.size());
        info.startIndex = startIndex;
        info.materialName = materialId >= 0 && materialId < static_cast<int>(materials.size())
            ? materials[materialId].name : "default";
        info.textureIndex = textureIndex;
        mSubMeshInfos.push_back(std::move(info));
    };

    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex)
    {
        const tinyobj::mesh_t& mesh = shapes[shapeIndex].mesh;
        std::unordered_map<int, std::vector<tinyobj::index_t>> facesByMaterial;
        size_t indexOffset = 0;
        for (size_t faceIndex = 0; faceIndex < mesh.num_face_vertices.size(); ++faceIndex)
        {
            const int materialId = mesh.material_ids[faceIndex];
            const unsigned char faceVertexCount = mesh.num_face_vertices[faceIndex];
            auto& destination = facesByMaterial[materialId];
            for (unsigned char vertexIndex = 0; vertexIndex < faceVertexCount; ++vertexIndex)
            {
                destination.push_back(mesh.indices[indexOffset + vertexIndex]);
            }
            indexOffset += faceVertexCount;
        }

        const std::string baseName = shapes[shapeIndex].name.empty()
            ? "submesh" : shapes[shapeIndex].name;
        for (const auto& [materialId, faceIndices] : facesByMaterial)
        {
            appendSubmesh(baseName, materialId, faceIndices);
        }
    }

    const UINT vertexByteSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    const UINT indexByteSize = static_cast<UINT>(indices.size() * sizeof(std::uint32_t));
    mGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(), mCommandList.Get(),
        vertices.data(), vertexByteSize, mGeo->VertexBufferUploader);
    mGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(), mCommandList.Get(),
        indices.data(), indexByteSize, mGeo->IndexBufferUploader);
    mGeo->VertexByteStride = sizeof(Vertex);
    mGeo->VertexBufferByteSize = vertexByteSize;
    mGeo->IndexFormat = DXGI_FORMAT_R32_UINT;
    mGeo->IndexBufferByteSize = indexByteSize;
    mUniqueTextureCount = static_cast<UINT>(mSubMeshTextures.size());
}

void App::BuildModelGeometryLegacy(std::string modelPath, std::string baseDir)
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
    mGeo->Name = "Model";

    std::vector<Vertex> allVertices;
    std::vector<std::uint32_t> allIndices;
    mSubMeshInfos.clear();

    // ============================================
    // 1. СОБИРАЕМ УНИКАЛЬНЫЕ МАТЕРИАЛЫ С ИХ ТЕКСТУРАМИ
    // ============================================
    std::unordered_map<std::string, int> materialToIndex;
    std::vector<SubMeshTextures> uniqueTextures;
    int nextTextureIndex = 0;

    OutputDebugStringA(("mat_size: " + std::to_string(materials.size()) + "\n").c_str());

    auto GetTexturePath = [&](const std::string& texName) -> std::wstring
        {
            if (texName.empty()) return L"";

            std::string basePath = baseDir + texName;

            // Заменяем расширение на .dds
            size_t dotPos = basePath.rfind('.');
            if (dotPos != std::string::npos)
            {
                std::string ddsPath = basePath.substr(0, dotPos) + ".dds";
                return std::wstring(ddsPath.begin(), ddsPath.end());
            }

            return std::wstring(basePath.begin(), basePath.end());
        };

    for (size_t i = 0; i < materials.size(); i++)
    {
        const auto& mat = materials[i];

        OutputDebugStringA(("Material: " + mat.name + "\n").c_str());
        OutputDebugStringA(("  diffuse_texname: " + mat.diffuse_texname + "\n").c_str());

        if (mat.diffuse_texname.empty()) continue;

        SubMeshTextures tex;

        // Альбедо
        tex.albedoPath = GetTexturePath(mat.diffuse_texname);

        // Нормаль (если есть)
        if (!mat.bump_texname.empty())
        {
            tex.normalPath = GetTexturePath(mat.bump_texname);
        }
        else if (!mat.normal_texname.empty())
        {
            tex.normalPath = GetTexturePath(mat.normal_texname);
        }

        // Высота / Displacement (если есть)
        // В OBJ обычно нет отдельного поля для height, используем bump_map или displacement_map
        if (!mat.displacement_texname.empty())
        {
            tex.heightPath = GetTexturePath(mat.displacement_texname);
        }
        else if (!mat.bump_texname.empty())
        {
            tex.heightPath = GetTexturePath(mat.bump_texname); // fallback, если height-карты нет
        }
        else
        {
            tex.heightPath = L"";
        }

        tex.arrayIndex = nextTextureIndex;
        materialToIndex[mat.name] = nextTextureIndex;
        uniqueTextures.push_back(tex);
        nextTextureIndex++;

        OutputDebugStringA(("  added texture index: " + std::to_string(nextTextureIndex - 1) + "\n").c_str());
    }

    OutputDebugStringA(("Unique textures count: " + std::to_string(uniqueTextures.size()) + "\n").c_str());

    // ============================================
    // 2. ЗАГРУЖАЕМ ГЕОМЕТРИЮ
    // ============================================
    for (size_t shapeIdx = 0; shapeIdx < shapes.size(); ++shapeIdx)
    {
        const auto& shape = shapes[shapeIdx];
        int materialId = shape.mesh.material_ids.empty() ? -1 : shape.mesh.material_ids[0];

        SubMeshInfo submeshInfo;
        submeshInfo.name = shape.name.empty()
            ? "submesh_" + std::to_string(shapeIdx)
            : shape.name;
        OutputDebugStringA(("Processing shape: " + submeshInfo.name + "\n").c_str());
        // Определяем текстуры для этого submesh
        if (materialId >= 0 && materialId < (int)materials.size())
        {
            submeshInfo.materialName = materials[materialId].name;
            OutputDebugStringA(("  material: " + submeshInfo.materialName + "\n").c_str());
            auto it = materialToIndex.find(submeshInfo.materialName);
            if (it != materialToIndex.end())
            {
                int texIndex = it->second;
                OutputDebugStringA(("  texIndex: " + std::to_string(texIndex) + "\n").c_str());

                // ============================================
                // ПРОВЕРКА: texIndex не должен выходить за границы
                // ============================================
                if (texIndex >= 0 && texIndex < (int)uniqueTextures.size())
                {
                    submeshInfo.textures = uniqueTextures[texIndex];
                    submeshInfo.textureIndex = texIndex;
                }
                else
                {
                    OutputDebugStringA(("  ERROR: texIndex " + std::to_string(texIndex) +
                        " out of range! uniqueTextures.size() = " + std::to_string(uniqueTextures.size()) + "\n").c_str());
                    submeshInfo.textureIndex = 0;
                }
            }
            else
            {
                OutputDebugStringA("  material not found in materialToIndex!\n");
                submeshInfo.textureIndex = 0;
            }
        }
        else
        {
            submeshInfo.materialName = "default";
            submeshInfo.textureIndex = 0;
            OutputDebugStringA("  no material, using default\n");
        }

        UINT startVertex = (UINT)allVertices.size();
        UINT startIndex = (UINT)allIndices.size();
        OutputDebugStringA(("  startVertex=" + std::to_string(startVertex) +
            ", startIndex=" + std::to_string(startIndex) + "\n").c_str());
        OutputDebugStringA(("  indices count=" + std::to_string(shape.mesh.indices.size()) + "\n").c_str());

        std::vector<XMFLOAT3> positions;
        std::vector<XMFLOAT2> texcoords;
        std::vector<UINT> indices;

        // Загружаем вершины
        for (const auto& index : shape.mesh.indices)
        {
            Vertex v;
            if (index.vertex_index >= 0 && index.vertex_index < (int)attrib.vertices.size() / 3)
            {
                v.Pos = {
                    attrib.vertices[3 * index.vertex_index + 0],
                    attrib.vertices[3 * index.vertex_index + 1],
                    attrib.vertices[3 * index.vertex_index + 2]
                };
            }
            else
            {
                OutputDebugStringA("  WARNING: invalid vertex index!\n");
                v.Pos = { 0.0f, 0.0f, 0.0f };
            }

            if (index.normal_index >= 0 && index.normal_index < (int)attrib.normals.size() / 3)
            {
                v.Normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2]
                };
            }
            else
            {
                v.Normal = { 0.0f, 1.0f, 0.0f };
            }

            if (index.texcoord_index >= 0 && index.texcoord_index < (int)attrib.texcoords.size() / 2)
            {
                v.TexCoord = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
                };
            }
            else
            {
                v.TexCoord = { 0.0f, 0.0f };
            }

            v.Tangent = { 0.0f, 0.0f, 0.0f };
            v.TexIndex = submeshInfo.textureIndex;

            allVertices.push_back(v);
            positions.push_back(v.Pos);
            texcoords.push_back(v.TexCoord);
        }

        OutputDebugStringA(("  vertices loaded: " + std::to_string(positions.size()) + "\n").c_str());

        for (size_t i = 0; i < shape.mesh.indices.size(); ++i) {
            allIndices.push_back(startVertex + (UINT)i);
            indices.push_back((UINT)i);
        }

        OutputDebugStringA(("  indices pushed: " + std::to_string(indices.size()) + "\n").c_str());

        // Вычисляем Tangent
        if (indices.size() >= 3)
        {
            for (size_t i = 0; i < indices.size(); i += 3)
            {
                UINT i0 = indices[i];
                UINT i1 = indices[i + 1];
                UINT i2 = indices[i + 2];

                XMFLOAT3 p0 = positions[i0];
                XMFLOAT3 p1 = positions[i1];
                XMFLOAT3 p2 = positions[i2];

                XMFLOAT2 uv0 = texcoords[i0];
                XMFLOAT2 uv1 = texcoords[i1];
                XMFLOAT2 uv2 = texcoords[i2];

                XMVECTOR deltaPos1 = XMLoadFloat3(&p1) - XMLoadFloat3(&p0);
                XMVECTOR deltaPos2 = XMLoadFloat3(&p2) - XMLoadFloat3(&p0);

                float deltaU1 = uv1.x - uv0.x;
                float deltaV1 = uv1.y - uv0.y;
                float deltaU2 = uv2.x - uv0.x;
                float deltaV2 = uv2.y - uv0.y;

                float r = 1.0f / (deltaU1 * deltaV2 - deltaU2 * deltaV1);
                XMVECTOR tangent = (deltaV2 * deltaPos1 - deltaV1 * deltaPos2) * r;
                tangent = XMVector3Normalize(tangent);

                for (int j = 0; j < 3; j++)
                {
                    UINT idx = indices[i + j];
                    XMVECTOR currentTangent = XMLoadFloat3(&allVertices[idx].Tangent);
                    currentTangent += tangent;
                    XMStoreFloat3(&allVertices[idx].Tangent, currentTangent);
                }
            }
        }
        else
        {
            OutputDebugStringA("  WARNING: not enough indices for tangents!\n");
        }

        // Нормализуем Tangent
        for (auto& v : allVertices)
        {
            XMVECTOR t = XMLoadFloat3(&v.Tangent);
            if (XMVector3Length(t).m128_f32[0] > 0.001f)
            {
                t = XMVector3Normalize(t);
                XMStoreFloat3(&v.Tangent, t);
            }
            else
            {
                v.Tangent = { 1.0f, 0.0f, 0.0f };
            }
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

    // Создаем GPU буферы
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

    mUniqueTextureCount = nextTextureIndex;

    // Сохраняем информацию о текстурах для загрузки
    mSubMeshTextures = uniqueTextures;
    OutputDebugStringA(("mSubMeshTextures.size() = " + std::to_string(mSubMeshTextures.size()) + "\n").c_str());

    // ============================================
    // ПРОВЕРКА: выводим все submesh и их индексы
    // ============================================
    for (size_t i = 0; i < mSubMeshInfos.size(); i++)
    {
        OutputDebugStringA(("SubMesh " + std::to_string(i) + ": " + mSubMeshInfos[i].name +
            " texIndex=" + std::to_string(mSubMeshInfos[i].textureIndex) + "\n").c_str());
    }
}

void App::LoadAllTextures()
{
    mTextures.clear();
    mTextures.reserve(mSubMeshTextures.size());

    const UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT textureIndex = 0; textureIndex < mSubMeshTextures.size(); ++textureIndex)
    {
        MeshTexture texture;
        texture.Filename = mSubMeshTextures[textureIndex].albedoPath;
        const HRESULT result = CreateDDSTextureFromFile12(md3dDevice.Get(), mCommandList.Get(),
            texture.Filename.c_str(), texture.Resource, texture.UploadHeap);
        if (FAILED(result))
        {
            throw std::runtime_error("Unable to load DDS texture for OBJ material.");
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = texture.Resource->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = texture.Resource->GetDesc().MipLevels;
        srv.Texture2D.ResourceMinLODClamp = 0.0f;

        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
            mCbvHeap->GetCPUDescriptorHandleForHeapStart(),
            kTextureSrvBase + textureIndex,
            descriptorSize);
        md3dDevice->CreateShaderResourceView(texture.Resource.Get(), &srv, handle);
        mTextures.push_back(std::move(texture));
    }
}