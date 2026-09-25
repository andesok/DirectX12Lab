#include "../headers/App.h"

#include <filesystem>
#include <cfloat>
#include <stdexcept>
#include <random>
#include <chrono>
#include <cstdio>

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
    if (!D3DApp::Initialize()) return false;

    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    BuildModelGeometry("Models/Sponza/sponza.obj", "Models/Sponza/");
    BuildScatterGeometry();   // куб для разбрасывания
    BuildSceneObjects();      // Sponza-сабмеши + тысячи кубов + окто-дерево

    BuildDescriptorHeaps();
    BuildConstantBuffers();
    BuildShadowResources();   // карта теней, SRV в слоте kShadowSrvSlot, буферы констант

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
    // Направление теперь нормализуется (UpdateSun). Раньше |{2,-1,0}| = 2.24
    // фактически усиливало свет, поэтому Strength немного подняли.
    dirLight.Strength = { 3.0f, 3.0f, 3.0f };
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
    mCamera->SetLens(kCameraFovY, AspectRatio(), kCameraNear, 10000.0f);

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
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 10000.0f);
    XMStoreFloat4x4(&mProj, P);

    if (mCamera)
    {
        mCamera->SetLens(kCameraFovY, AspectRatio(), kCameraNear, 10000.0f);
    }
}

void App::Update(const GameTimer& gt)
{
    if (!mCamera) return;
    float dt = gt.DeltaTime();
    float speed = 800.0f * dt;

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

    // ===== Переключатели ДЗ №4 =====
    if (WasKeyPressed('C')) mFrustumCullingEnabled = !mFrustumCullingEnabled;
    if (WasKeyPressed('O')) mUseOctree = !mUseOctree;
    if (WasKeyPressed('F')) mFreezeFrustum = !mFreezeFrustum;

    XMMATRIX view = mCamera->GetView();
    XMMATRIX proj = mCamera->GetProj();

    // Отсечение + заполнение константных буферов только для видимых объектов
    UpdateCulling(gt.TotalTime());

    // ===== Каскадные тени =====
    if (WasKeyPressed('H')) mShadowsEnabled = !mShadowsEnabled;
    if (WasKeyPressed('V')) mShowCascades = !mShowCascades;
    if (WasKeyPressed('P')) mPcfRadius = (mPcfRadius + 1) % 4;
    if (WasKeyPressed(VK_OEM_4)) mCascadeLambda = (std::max)(0.0f, mCascadeLambda - 0.1f); // [
    if (WasKeyPressed(VK_OEM_6)) mCascadeLambda = (std::min)(1.0f, mCascadeLambda + 0.1f); // ]

    UpdateSun(dt);
    UpdateShadows();

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

    // ============================================
    // 0. SHADOW PASS: по проходу глубины на каскад
    // ============================================
    if (mShadowsEnabled)
    {
        mShadowMap->TransitionToDepthWrite(mCommandList.Get());

        const UINT passCBSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ShadowPassConstants));
        const D3D12_GPU_VIRTUAL_ADDRESS passCBBase = mShadowPassCB->Resource()->GetGPUVirtualAddress();

        for (UINT c = 0; c < kCascadeCount; ++c)
        {
            mRenderSystem->DrawShadowCascade(mCommandList.Get(),
                mShadowMap->Dsv(c),
                mShadowMap->Viewport(),
                mShadowMap->Scissor(),
                passCBBase + (UINT64)c * passCBSize,
                mShadowDrawLists[c].data(),
                (UINT)mShadowDrawLists[c].size());
        }

        mShadowMap->TransitionToShaderResource(mCommandList.Get());
    }

    mRenderSystem->BeginFrame(mCommandList.Get(), mScreenViewport, mScissorRect,
        mGBuffer.get(), DepthStencilView());

    ID3D12DescriptorHeap* heaps[] = { mCbvHeap.Get(), mSamplerHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(heaps), heaps);

    UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Рисуем только то, что прошло отсечение (список собран в UpdateCulling)
    mRenderSystem->DrawItems(mCommandList.Get(),
        mDrawList.data(), (UINT)mDrawList.size(),
        mCbvHeap.Get(), mSamplerHeap.Get());

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
        (UINT)mLights.size(),
        mShadowMap->Srv(),
        mCascadeCB->Resource()->GetGPUVirtualAddress());

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
    // По одному 256-байтному слоту на КАЖДЫЙ объект сцены
    const UINT objectCount = (std::max)(1u, (UINT)mSceneObjects.size());
    mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(md3dDevice.Get(), objectCount, true);

    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

    D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mObjectCB->Resource()->GetGPUVirtualAddress();
    // Offset to the ith object constant buffer in the buffer.
    int boxCBufIndex = 0;
    cbAddress += boxCBufIndex * objCBByteSize;

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
    mSceneMin = minimum;
    mSceneMax = maximum;

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

            SubmeshGeometry submesh;
            submesh.IndexCount = static_cast<UINT>(faceIndices.size());
            submesh.StartIndexLocation = startIndex;
            submesh.BaseVertexLocation = 0;
            // AABB сабмеша по его вершинам (Sponza не трансформируется => это мировой AABB)
            BoundingBox::CreateFromPoints(submesh.Bounds, faceIndices.size(),
                &vertices[startVertex].Pos, sizeof(Vertex));
            mGeo->DrawArgs[uniqueName] = submesh;

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

// =====================================================================
// ДЗ №4
// =====================================================================

bool App::WasKeyPressed(int vkey)
{
    // Срабатывает один раз в момент нажатия (а не каждый кадр, пока держим)
    const bool down = d3dUtil::IsKeyDown(vkey);
    const bool pressed = down && !mKeyWasDown[vkey & 0xFF];
    mKeyWasDown[vkey & 0xFF] = down;
    return pressed;
}

void App::BuildScatterGeometry()
{
    // Единичный куб [-0.5; 0.5]^3, 24 вершины (у каждой грани свои нормали/UV)
    struct Face { XMFLOAT3 n, u, v; };
    const Face faces[6] = {
        { { 0, 0, -1 }, {  1, 0, 0 }, { 0, 1, 0 } },
        { { 0, 0,  1 }, { -1, 0, 0 }, { 0, 1, 0 } },
        { { -1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
        { {  1, 0, 0 }, { 0, 0,  1 }, { 0, 1, 0 } },
        { { 0,  1, 0 }, { 1, 0, 0 }, { 0, 0,  1 } },
        { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
    };

    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    for (const Face& f : faces)
    {
        const XMVECTOR n = XMLoadFloat3(&f.n);
        const XMVECTOR u = XMLoadFloat3(&f.u);
        const XMVECTOR v = XMLoadFloat3(&f.v);
        const XMVECTOR c = n * 0.5f;

        const float su[4] = { -0.5f, -0.5f,  0.5f, 0.5f };
        const float sv[4] = { -0.5f,  0.5f,  0.5f, -0.5f };
        const XMFLOAT2 uv[4] = { {0, 1}, {0, 0}, {1, 0}, {1, 1} };

        const std::uint32_t base = (std::uint32_t)vertices.size();
        for (int i = 0; i < 4; ++i)
        {
            Vertex vert = {};
            XMStoreFloat3(&vert.Pos, c + u * su[i] + v * sv[i]);
            vert.Normal = f.n;
            vert.TexCoord = uv[i];
            vert.Tangent = f.u;
            vert.TexIndex = 0;
            vertices.push_back(vert);
        }
        // Обход по часовой стрелке при взгляде снаружи (front face в D3D)
        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }

    mScatterGeo = std::make_unique<MeshGeometry>();
    mScatterGeo->Name = "ScatterCube";

    const UINT vbByteSize = (UINT)(vertices.size() * sizeof(Vertex));
    const UINT ibByteSize = (UINT)(indices.size() * sizeof(std::uint32_t));

    mScatterGeo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(), mCommandList.Get(),
        vertices.data(), vbByteSize, mScatterGeo->VertexBufferUploader);
    mScatterGeo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(), mCommandList.Get(),
        indices.data(), ibByteSize, mScatterGeo->IndexBufferUploader);

    mScatterGeo->VertexByteStride = sizeof(Vertex);
    mScatterGeo->VertexBufferByteSize = vbByteSize;
    mScatterGeo->IndexFormat = DXGI_FORMAT_R32_UINT;
    mScatterGeo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry cube;
    cube.IndexCount = (UINT)indices.size();
    cube.StartIndexLocation = 0;
    cube.BaseVertexLocation = 0;
    cube.Bounds = BoundingBox(XMFLOAT3(0, 0, 0), XMFLOAT3(0.5f, 0.5f, 0.5f));
    mScatterGeo->DrawArgs["cube"] = cube;
}

void App::BuildSceneObjects()
{
    mSceneObjects.clear();
    mSceneObjects.reserve(mSubMeshInfos.size() + kScatterCount);

    // ---- 1. Сабмеши Sponza — тоже отдельные объекты для отсечения ----
    for (const SubMeshInfo& info : mSubMeshInfos)
    {
        const SubmeshGeometry& sm = mGeo->DrawArgs[info.name];

        SceneObject obj;
        obj.Mesh = mGeo.get();
        obj.IndexCount = sm.IndexCount;
        obj.StartIndexLocation = sm.StartIndexLocation;
        obj.BaseVertexLocation = sm.BaseVertexLocation;
        obj.SRVIndex = kTextureSrvBase + info.textureIndex;
        obj.World = MathHelper::Identity4x4();
        obj.Bounds = sm.Bounds;
        obj.IsWell = info.name.find("Well") != std::string::npos;
        mSceneObjects.push_back(obj);
    }

    // ---- 2. Тысячи кубов, случайно раскиданных внутри Sponza ----
    std::mt19937 rng(12345); // фиксированное зерно => одинаковая сцена при каждом запуске
    const XMFLOAT3 size = {
        mSceneMax.x - mSceneMin.x,
        mSceneMax.y - mSceneMin.y,
        mSceneMax.z - mSceneMin.z };

    std::uniform_real_distribution<float> distX(mSceneMin.x + 0.05f * size.x, mSceneMax.x - 0.05f * size.x);
    std::uniform_real_distribution<float> distY(mSceneMin.y + 0.02f * size.y, mSceneMin.y + 0.75f * size.y);
    std::uniform_real_distribution<float> distZ(mSceneMin.z + 0.05f * size.z, mSceneMax.z - 0.05f * size.z);
    std::uniform_real_distribution<float> distScale(mSceneRadius * 0.004f, mSceneRadius * 0.012f);
    std::uniform_real_distribution<float> distAngle(0.0f, XM_2PI);
    std::uniform_int_distribution<int> distTex(0, (std::max)(0, mUniqueTextureCount - 1));

    const SubmeshGeometry& cube = mScatterGeo->DrawArgs["cube"];

    for (UINT i = 0; i < kScatterCount; ++i)
    {
        const float scale = distScale(rng);
        const XMMATRIX world =
            XMMatrixScaling(scale, scale, scale) *
            XMMatrixRotationRollPitchYaw(distAngle(rng), distAngle(rng), distAngle(rng)) *
            XMMatrixTranslation(distX(rng), distY(rng), distZ(rng));

        SceneObject obj;
        obj.Mesh = mScatterGeo.get();
        obj.IndexCount = cube.IndexCount;
        obj.StartIndexLocation = cube.StartIndexLocation;
        obj.BaseVertexLocation = cube.BaseVertexLocation;
        obj.SRVIndex = kTextureSrvBase + distTex(rng);
        XMStoreFloat4x4(&obj.World, world);

        // Локальный AABB -> мировой AABB (Transform пересчитывает AABB по 8 углам)
        cube.Bounds.Transform(obj.Bounds, world);
        mSceneObjects.push_back(obj);
    }

    // ---- 3. Окто-дерево по мировым AABB ----
    std::vector<BoundingBox> bounds;
    bounds.reserve(mSceneObjects.size());
    for (const SceneObject& obj : mSceneObjects)
        bounds.push_back(obj.Bounds);

    const auto t0 = std::chrono::high_resolution_clock::now();
    mOctree.Build(bounds, /*maxDepth*/ 8, /*maxObjectsPerNode*/ 16);
    const auto t1 = std::chrono::high_resolution_clock::now();

    const double buildMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    OutputDebugStringA(("Octree: objects=" + std::to_string(mSceneObjects.size()) +
        " nodes=" + std::to_string(mOctree.NodeCount()) +
        " build=" + std::to_string(buildMs) + " ms\n").c_str());

    mVisibleObjects.reserve(mSceneObjects.size());
    mDrawList.reserve(mSceneObjects.size());
}

void App::UpdateCulling(float totalTime)
{
    const XMMATRIX view = mCamera->GetView();
    const XMMATRIX proj = mCamera->GetProj();
    const XMMATRIX viewProj = view * proj;

    // Пирамида для отсечения. При «заморозке» остаётся старая —
    // можно отлететь и посмотреть, что рисуется только её содержимое.
    if (!mFreezeFrustum)
        XMStoreFloat4x4(&mFrozenViewProj, viewProj);

    const auto t0 = std::chrono::high_resolution_clock::now();

    mCullFrustum.ExtractFromViewProj(XMLoadFloat4x4(&mFrozenViewProj));
    mVisibleObjects.clear();
    mCullStats = {};

    const UINT total = (UINT)mSceneObjects.size();

    if (!mFrustumCullingEnabled)
    {
        // Отсечение выключено — рисуем всё
        for (UINT i = 0; i < total; ++i)
            mVisibleObjects.push_back(i);
        mCullStats.Visible = total;
    }
    else if (mUseOctree)
    {
        // Иерархическое отсечение через окто-дерево
        mOctree.Query(mCullFrustum, mVisibleObjects, mCullStats);
    }
    else
    {
        // Полный перебор: каждый объект против 6 плоскостей
        for (UINT i = 0; i < total; ++i)
        {
            ++mCullStats.ObjectTests;
            if (mCullFrustum.IsVisible(mSceneObjects[i].Bounds))
                mVisibleObjects.push_back(i);
        }
        mCullStats.Visible = (UINT)mVisibleObjects.size();
    }

    const auto t1 = std::chrono::high_resolution_clock::now();
    mCullTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // ---- Константы и список отрисовки только для видимых ----
    // GPU простаивает (Draw() заканчивается FlushCommandQueue), поэтому
    // перезаписывать upload-буфер здесь безопасно.
    const UINT cbByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    const D3D12_GPU_VIRTUAL_ADDRESS cbBase = mObjectCB->Resource()->GetGPUVirtualAddress();

    mDrawList.clear();
    UINT slot = 0;
    for (UINT idx : mVisibleObjects)
    {
        const SceneObject& obj = mSceneObjects[idx];
        if (mEnableTessellation && obj.IsWell)
            continue; // колодец рисует проход тесселяции

        const XMMATRIX world = XMLoadFloat4x4(&obj.World);

        ObjectConstants c;
        XMStoreFloat4x4(&c.WorldViewProj, XMMatrixTranspose(world * viewProj));
        XMStoreFloat4x4(&c.World, XMMatrixTranspose(world));
        c.gTime = totalTime;
        mObjectCB->CopyData(slot, c);

        RenderItem item;
        item.Mesh = obj.Mesh;
        item.IndexCount = obj.IndexCount;
        item.StartIndexLocation = obj.StartIndexLocation;
        item.BaseVertexLocation = obj.BaseVertexLocation;
        item.SRVIndex = obj.SRVIndex;
        item.CBAddress = cbBase + (UINT64)slot * cbByteSize;
        mDrawList.push_back(item);

        ++slot;
    }

    // ---- Статистика в заголовок окна (обновляется раз в секунду вместе с fps) ----
    wchar_t caption[256];
    swprintf_s(caption, L"HW4 | Culling[C]: %s | Octree[O]: %s | Freeze[F]: %s | Visible: %u/%u | NodeTests: %u | ObjTests: %u | Cull: %.3f ms",
        mFrustumCullingEnabled ? L"ON" : L"OFF",
        mUseOctree ? L"ON" : L"OFF",
        mFreezeFrustum ? L"ON" : L"OFF",
        mCullStats.Visible, total,
        mCullStats.NodeTests, mCullStats.ObjectTests,
        mCullTimeMs);
    mMainWndCaption = caption;
}


// =====================================================================
// ДЗ №5: КАСКАДНЫЕ КАРТЫ ТЕНЕЙ
// =====================================================================

void App::BuildShadowResources()
{
    mShadowMap = std::make_unique<CascadedShadowMap>(md3dDevice.Get(), kShadowMapSize, kCascadeCount);

    const UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mShadowMap->BuildSrv(
        CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvHeap->GetCPUDescriptorHandleForHeapStart(), kShadowSrvSlot, descriptorSize),
        CD3DX12_GPU_DESCRIPTOR_HANDLE(mCbvHeap->GetGPUDescriptorHandleForHeapStart(), kShadowSrvSlot, descriptorSize));

    // Мировые матрицы всех объектов — один раз (сцена статична).
    // Индекс слота = индекс объекта, поэтому любой каскад может нарисовать любой объект.
    const UINT objectCount = (std::max)(1u, (UINT)mSceneObjects.size());
    mShadowObjectCB = std::make_unique<UploadBuffer<ShadowObjectConstants>>(md3dDevice.Get(), objectCount, true);
    for (UINT i = 0; i < (UINT)mSceneObjects.size(); ++i)
    {
        ShadowObjectConstants c;
        XMStoreFloat4x4(&c.World, XMMatrixTranspose(XMLoadFloat4x4(&mSceneObjects[i].World)));
        mShadowObjectCB->CopyData(i, c);
    }

    mShadowPassCB = std::make_unique<UploadBuffer<ShadowPassConstants>>(md3dDevice.Get(), kCascadeCount, true);
    mCascadeCB = std::make_unique<UploadBuffer<CascadeConstants>>(md3dDevice.Get(), 1, true);

    for (auto& list : mShadowDrawLists)
        list.reserve(mSceneObjects.size());
    mShadowCasters.reserve(mSceneObjects.size());
}

void App::UpdateSun(float dt)
{
    const float rotSpeed = 0.8f * dt;
    if (d3dUtil::IsKeyDown(VK_LEFT))  mSunAzimuth -= rotSpeed;
    if (d3dUtil::IsKeyDown(VK_RIGHT)) mSunAzimuth += rotSpeed;
    if (d3dUtil::IsKeyDown(VK_UP))    mSunElevation += rotSpeed;
    if (d3dUtil::IsKeyDown(VK_DOWN))  mSunElevation -= rotSpeed;
    mSunElevation = (std::max)(0.15f, (std::min)(1.5f, mSunElevation));

    // Направление, КУДА светит солнце (вниз)
    const float ce = cosf(mSunElevation);
    XMVECTOR dir = XMVector3Normalize(XMVectorSet(
        ce * cosf(mSunAzimuth), -sinf(mSunElevation), ce * sinf(mSunAzimuth), 0.0f));
    XMStoreFloat3(&mSunDir, dir);

    if (!mLights.empty())
        mLights[0].Direction = mSunDir;   // тот же вектор уходит в StructuredBuffer света
}

void App::UpdateShadows()
{
    if (!mCamera || !mShadowMap) return;

    // ---- 1. Разбиение на каскады и матрицы света ----
    const BoundingBox sceneBounds = [this]()
        {
            BoundingBox b;
            BoundingBox::CreateFromPoints(b, XMLoadFloat3(&mSceneMin), XMLoadFloat3(&mSceneMax));
            return b;
        }();

    mShadowMap->UpdateCascades(mCamera->GetView(), kCameraFovY, AspectRatio(), kCameraNear,
        mShadowNear, mShadowFar, mCascadeLambda, mSunDir, sceneBounds);

    CascadeConstants cc;
    float splits[4] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
    float texels[4] = { 0, 0, 0, 0 };

    const UINT objCBSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ShadowObjectConstants));
    const D3D12_GPU_VIRTUAL_ADDRESS objCBBase = mShadowObjectCB->Resource()->GetGPUVirtualAddress();

    for (UINT c = 0; c < kCascadeCount; ++c)
    {
        const XMMATRIX lightViewProj = XMLoadFloat4x4(&mShadowMap->LightViewProj(c));

        ShadowPassConstants sp;
        XMStoreFloat4x4(&sp.LightViewProj, XMMatrixTranspose(lightViewProj));
        mShadowPassCB->CopyData(c, sp);

        cc.LightViewProj[c] = sp.LightViewProj;
        splits[c] = mShadowMap->SplitFar(c);
        texels[c] = mShadowMap->TexelWorldSize(c);

        // ---- 2. Отсечение отбрасывающих тень (переиспользуем ДЗ №4) ----
        mShadowDrawLists[c].clear();
        mShadowCasterCount[c] = 0;
        if (!mShadowsEnabled) continue;

        mShadowCasters.clear();
        if (!mFrustumCullingEnabled)
        {
            for (UINT i = 0; i < (UINT)mSceneObjects.size(); ++i)
                mShadowCasters.push_back(i);
        }
        else
        {
            // Ортопроекция тоже даёт z в [0, w] — Frustum работает без изменений
            Frustum lightFrustum;
            lightFrustum.ExtractFromViewProj(lightViewProj);

            if (mUseOctree)
            {
                CullStats stats;
                mOctree.Query(lightFrustum, mShadowCasters, stats);
            }
            else
            {
                for (UINT i = 0; i < (UINT)mSceneObjects.size(); ++i)
                    if (lightFrustum.IsVisible(mSceneObjects[i].Bounds))
                        mShadowCasters.push_back(i);
            }
        }

        for (UINT idx : mShadowCasters)
        {
            const SceneObject& obj = mSceneObjects[idx];
            RenderItem item;
            item.Mesh = obj.Mesh;
            item.IndexCount = obj.IndexCount;
            item.StartIndexLocation = obj.StartIndexLocation;
            item.BaseVertexLocation = obj.BaseVertexLocation;
            item.CBAddress = objCBBase + (UINT64)idx * objCBSize;
            mShadowDrawLists[c].push_back(item);
        }
        mShadowCasterCount[c] = (UINT)mShadowDrawLists[c].size();
    }

    // ---- 3. Константы для прохода освещения ----
    for (UINT c = kCascadeCount; c < 4; ++c)
        cc.LightViewProj[c] = MathHelper::Identity4x4();

    cc.SplitDepths = { splits[0], splits[1], splits[2], splits[3] };
    cc.TexelWorldSize = { texels[0], texels[1], texels[2], texels[3] };
    cc.LightDir = mSunDir;
    cc.ShadowTexelUV = 1.0f / (float)kShadowMapSize;
    cc.ShadowsEnabled = mShadowsEnabled ? 1 : 0;
    cc.PcfRadius = mPcfRadius;
    cc.ShowCascades = mShowCascades ? 1 : 0;
    cc.CascadeCount = (int)kCascadeCount;
    mCascadeCB->CopyData(0, cc);

    // ---- 4. Статистика в заголовок ----
    wchar_t buf[256];
    swprintf_s(buf, L" || Shadows[H]: %s | PCF[P]: %s | Lambda[ ]: %.1f | Cascades[V] | Casters: %u/%u/%u/%u | Splits: %.0f/%.0f/%.0f/%.0f",
        mShadowsEnabled ? L"ON" : L"OFF",
        mPcfRadius == 0 ? L"off" : (mPcfRadius == 1 ? L"3x3" : (mPcfRadius == 2 ? L"5x5" : L"7x7")),
        mCascadeLambda,
        mShadowCasterCount[0], mShadowCasterCount[1], mShadowCasterCount[2], mShadowCasterCount[3],
        splits[0], splits[1], splits[2], splits[3]);
    mMainWndCaption += buf;
}