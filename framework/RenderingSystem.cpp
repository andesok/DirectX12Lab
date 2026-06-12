#include "../framework/RenderingSystem.h"

void RenderingSystem::Initialize() {
    BuildRootSignature();
    BuildShadersAndInputLayout();
    BuildPSO();
    BuildWavePSO();
    BuildDeferredRootSignature();
    BuildDeferredShaders();
    BuildDeferredPSO();
}

RenderingSystem::RenderingSystem(
    ID3D12Device* device,
    const DXGI_FORMAT gBufferFormats[3],
    DXGI_FORMAT depthBufferFormat,
    bool msaaState,
    UINT msaaQuality)
    : md3dDevice(device),
    mDepthStencilFormat(depthBufferFormat),
    mMsaaState(msaaState),
    mMsaaQuality(msaaQuality)
{
    for (int i = 0; i < 3; i++)
    {
        mFormats[i] = gBufferFormats[i];
    }
}

RenderingSystem::~RenderingSystem() {}

void RenderingSystem::BuildRootSignature()
{
    CD3DX12_ROOT_PARAMETER slotRootParameter[3];

    CD3DX12_DESCRIPTOR_RANGE cbvTable;
    cbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
    slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable);

    CD3DX12_DESCRIPTOR_RANGE srvTable;
    srvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    slotRootParameter[1].InitAsDescriptorTable(1, &srvTable);

    CD3DX12_DESCRIPTOR_RANGE samplerTable;
    samplerTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);
    slotRootParameter[2].InitAsDescriptorTable(1, &samplerTable);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature)));
}

void RenderingSystem::BuildShadersAndInputLayout()
{
    mvsByteCode = d3dUtil::CompileShader(L"Shaders\\texture_vs.hlsl", nullptr, "VS", "vs_5_0");
    mpsByteCode = d3dUtil::CompileShader(L"Shaders\\texture_ps.hlsl", nullptr, "PS", "ps_5_0");
    mWaveVSByteCode = d3dUtil::CompileShader(L"Shaders\\texture_wave_vs.hlsl", nullptr, "main", "vs_5_0");

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
}

void RenderingSystem::BuildPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
    ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

    psoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mvsByteCode->GetBufferPointer()),
        mvsByteCode->GetBufferSize()
    };
    psoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mpsByteCode->GetBufferPointer()),
        mpsByteCode->GetBufferSize()
    };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.SampleDesc.Count = mMsaaState ? 4 : 1;
    psoDesc.SampleDesc.Quality = mMsaaState ? (mMsaaQuality - 1) : 0;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = mFormats[0];
    psoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));
}

void RenderingSystem::BuildWavePSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
    ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

    psoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mWaveVSByteCode->GetBufferPointer()),
        mWaveVSByteCode->GetBufferSize()
    };
    psoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mpsByteCode->GetBufferPointer()),
        mpsByteCode->GetBufferSize()
    };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.SampleDesc.Count = mMsaaState ? 4 : 1;
    psoDesc.SampleDesc.Quality = mMsaaState ? (mMsaaQuality - 1) : 0;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = mFormats[0];
    psoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mWavePSO)));
}

void RenderingSystem::BeginFrame(ID3D12GraphicsCommandList* cmdList,
    D3D12_VIEWPORT& viewport,
    D3D12_RECT& scissor,
    GBuffer* gBuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv)
{
    if (!gBuffer) return;
    gBuffer->TransitionToRenderTarget(cmdList);
    gBuffer->SetAsRenderTargets(cmdList, &dsv);
    gBuffer->Clear(cmdList);
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);
    cmdList->SetGraphicsRootSignature(mRootSignature.Get());
    mViewport = viewport;
    mScissor = scissor;
}

void RenderingSystem::DrawItem(ID3D12GraphicsCommandList* cmdList,
    const RenderItem& item,
    ID3D12DescriptorHeap* mainHeap,
    ID3D12DescriptorHeap* samplerHeap)
{
    ID3D12PipelineState* pso = (item.Shader == ShaderType::WAVE) ? mWavePSO.Get() : mPSO.Get();
    cmdList->SetPipelineState(pso);

    ID3D12DescriptorHeap* heaps[] = { mainHeap, samplerHeap };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    auto handle = mainHeap->GetGPUDescriptorHandleForHeapStart();
    UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    auto cbvHandle = handle;
    cbvHandle.ptr += item.CBIndex * descriptorSize;
    cmdList->SetGraphicsRootDescriptorTable(0, cbvHandle);

    auto srvHandle = handle;
    srvHandle.ptr += item.SRVIndex * descriptorSize;
    cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);

    cmdList->SetGraphicsRootDescriptorTable(2, samplerHeap->GetGPUDescriptorHandleForHeapStart());

    auto vbv = item.Mesh->VertexBufferView();
    auto ibv = item.Mesh->IndexBufferView();
    cmdList->IASetVertexBuffers(0, 1, &vbv);
    cmdList->IASetIndexBuffer(&ibv);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    auto& submesh = item.Mesh->DrawArgs[item.SubmeshName];
    cmdList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.StartIndexLocation, submesh.BaseVertexLocation, 0);
}

void RenderingSystem::BuildDeferredRootSignature()
{
    // Root signature для deferred lighting pass
    CD3DX12_ROOT_PARAMETER slotRootParameter[3];

    // 3 текстуры G-Buffer (Albedo, Normal, Position)
    CD3DX12_DESCRIPTOR_RANGE srvTable;
    srvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0); // t0, t1, t2
    slotRootParameter[0].InitAsDescriptorTable(1, &srvTable);

    CD3DX12_DESCRIPTOR_RANGE cbvTable;
    cbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0); // b0
    slotRootParameter[1].InitAsDescriptorTable(1, &cbvTable);

    // Sampler
    CD3DX12_DESCRIPTOR_RANGE samplerTable;
    samplerTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);
    slotRootParameter[2].InitAsDescriptorTable(1, &samplerTable);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);
    ThrowIfFailed(md3dDevice->CreateRootSignature(0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mDeferredRootSig)));
}

void RenderingSystem::BuildDeferredShaders()
{
    mDeferredVSByteCode = d3dUtil::CompileShader(L"Shaders\\fullscreen_vs.hlsl",
        nullptr, "main", "vs_5_0");
    mDeferredPSByteCode = d3dUtil::CompileShader(L"Shaders\\deferred_ps.hlsl",
        nullptr, "main", "ps_5_0");
}

void RenderingSystem::BuildDeferredPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

    psoDesc.InputLayout = { nullptr, 0 }; // Нет вершинных буферов (full-screen quad)
    psoDesc.pRootSignature = mDeferredRootSig.Get();
    psoDesc.VS = { mDeferredVSByteCode->GetBufferPointer(), mDeferredVSByteCode->GetBufferSize() };
    psoDesc.PS = { mDeferredPSByteCode->GetBufferPointer(), mDeferredPSByteCode->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // Back buffer format

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mDeferredPSO)));
}

void RenderingSystem::EndFrame(ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* cbvHeap,
    ID3D12DescriptorHeap* samplerHeap,
    UINT descriptorSize,
    GBuffer* gBuffer)
{
    if (!gBuffer) return;

    gBuffer->TransitionToShaderResource(cmdList);

    // Биндим обе heap
    ID3D12DescriptorHeap* heaps[] = { cbvHeap, samplerHeap };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    cmdList->SetGraphicsRootSignature(mDeferredRootSig.Get());
    cmdList->SetPipelineState(mDeferredPSO.Get());

    // Слот 0: G-Buffer SRV (смещение 2 = kGBufferSrvBase)
    CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(
        cbvHeap->GetGPUDescriptorHandleForHeapStart());
    srvHandle.Offset(2, descriptorSize);
    cmdList->SetGraphicsRootDescriptorTable(0, srvHandle);

    // Слот 1: CBV (слот 0 в куче)
    CD3DX12_GPU_DESCRIPTOR_HANDLE cbvHandle(
        cbvHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetGraphicsRootDescriptorTable(1, cbvHandle);

    // Слот 2: Sampler
    cmdList->SetGraphicsRootDescriptorTable(2,
        samplerHeap->GetGPUDescriptorHandleForHeapStart());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->RSSetViewports(1, &mViewport);
    cmdList->RSSetScissorRects(1, &mScissor);
    cmdList->DrawInstanced(3, 1, 0, 0);
}