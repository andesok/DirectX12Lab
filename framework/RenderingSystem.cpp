#include "../framework/RenderingSystem.h"

void RenderingSystem::Initialize() {
    BuildRootSignature();
    BuildGeometryPSO();
    BuildDeferredRootSignature();
    BuildDeferredShaders();
    BuildDeferredPSO();
    BuildTessellationRootSignature();
    BuildTessellationPSO();
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

void RenderingSystem::BeginFrame(ID3D12GraphicsCommandList* cmdList,
    D3D12_VIEWPORT& viewport,
    D3D12_RECT& scissor,
    GBuffer* gBuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv)
{
    gBuffer->TransitionToRenderTarget(cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
    gBuffer->GetRtv(0),
    gBuffer->GetRtv(1),
    gBuffer->GetRtv(2)
    };
    cmdList->OMSetRenderTargets(3, rtvs, false, &dsv);

    float clearAlbedo[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float clearNormal[4] = { 0.5f, 0.5f, 1.0f, 0.0f };
    float clearPosition[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    cmdList->ClearRenderTargetView(gBuffer->GetRtv(0), clearAlbedo, 0, nullptr);
    cmdList->ClearRenderTargetView(gBuffer->GetRtv(1), clearNormal, 0, nullptr);
    cmdList->ClearRenderTargetView(gBuffer->GetRtv(2), clearPosition, 0, nullptr);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);

    cmdList->SetPipelineState(mGeometryPSO.Get());
    cmdList->SetGraphicsRootSignature(mRootSignature.Get());

    mViewport = viewport;
    mScissor = scissor;

}

void RenderingSystem::DrawItem(ID3D12GraphicsCommandList* cmdList,
    const RenderItem& item,
    ID3D12DescriptorHeap* mainHeap,
    ID3D12DescriptorHeap* samplerHeap,
    bool isGeometryPass)
{
    // ============================================
    // 1. ВЫБИРАЕМ PSO И ROOT SIGNATURE
    // ============================================
    ID3D12PipelineState* pso = nullptr;
    ID3D12RootSignature* rootSig = nullptr;

    if (isGeometryPass && item.UseTessellation)
    {
        pso = mTessellationPSO.Get();
        rootSig = mTessellationRootSig.Get();
    }
    else if (isGeometryPass)
    {
        pso = mGeometryPSO.Get();
        rootSig = mRootSignature.Get();
    }
    else
    {
        pso = mGeometryPSO.Get();
        rootSig = mRootSignature.Get();
    }

    cmdList->SetPipelineState(pso);
    cmdList->SetGraphicsRootSignature(rootSig);

    // ============================================
    // 2. ПРИВЯЗЫВАЕМ КУЧИ ДЕСКРИПТОРОВ
    // ============================================
    ID3D12DescriptorHeap* heaps[] = { mainHeap, samplerHeap };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    auto handle = mainHeap->GetGPUDescriptorHandleForHeapStart();

    // ============================================
    // 3. ПРИВЯЗЫВАЕМ РЕСУРСЫ
    // ============================================
    if (item.UseTessellation)
    {
        // Слот 0: CBV (уже привязан в App::Draw)
        // Слот 1: Текстуры (Descriptor Table)
        auto srvHandle = handle;
        srvHandle.ptr += item.SRVIndex * descriptorSize;
        cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);

        // Слот 2: Sampler (Descriptor Table)
        cmdList->SetGraphicsRootDescriptorTable(2,
            samplerHeap->GetGPUDescriptorHandleForHeapStart());
    }
    else
    {
        // Слот 0: CBV
        auto cbvHandle = handle;
        cbvHandle.ptr += item.CBIndex * descriptorSize;
        cmdList->SetGraphicsRootDescriptorTable(0, cbvHandle);

        // Слот 1: SRV
        auto srvHandle = handle;
        srvHandle.ptr += item.SRVIndex * descriptorSize;
        cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);

        // Слот 2: Sampler
        cmdList->SetGraphicsRootDescriptorTable(2,
            samplerHeap->GetGPUDescriptorHandleForHeapStart());
    }

    // ============================================
    // 4. ПРИВЯЗЫВАЕМ ГЕОМЕТРИЮ
    // ============================================
    auto vbv = item.Mesh->VertexBufferView();
    auto ibv = item.Mesh->IndexBufferView();
    cmdList->IASetVertexBuffers(0, 1, &vbv);
    cmdList->IASetIndexBuffer(&ibv);

    // ============================================
    // 5. ТОПОЛОГИЯ
    // ============================================
    if (item.UseTessellation)
    {
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    }
    else
    {
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    // ============================================
    // 6. РИСУЕМ
    // ============================================
    auto& submesh = item.Mesh->DrawArgs[item.SubmeshName];
    cmdList->DrawIndexedInstanced(
        submesh.IndexCount,
        1,
        submesh.StartIndexLocation,
        submesh.BaseVertexLocation,
        0);
}

void RenderingSystem::BuildDeferredRootSignature()
{
    // Root signature для deferred lighting pass
    CD3DX12_ROOT_PARAMETER slotRootParameter[4];

    // 3 текстуры G-Buffer
    CD3DX12_DESCRIPTOR_RANGE srvTable;
    srvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);
    slotRootParameter[0].InitAsDescriptorTable(1, &srvTable);

    slotRootParameter[1].InitAsConstantBufferView(0);

    slotRootParameter[2].InitAsShaderResourceView(3);

    // Sampler
    CD3DX12_DESCRIPTOR_RANGE samplerTable;
    samplerTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);
    slotRootParameter[3].InitAsDescriptorTable(1, &samplerTable);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter, 0, nullptr,
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

    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.pRootSignature = mDeferredRootSig.Get();
    psoDesc.VS = { mDeferredVSByteCode->GetBufferPointer(), mDeferredVSByteCode->GetBufferSize() };
    psoDesc.PS = { mDeferredPSByteCode->GetBufferPointer(), mDeferredPSByteCode->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mDeferredPSO)));
}

void RenderingSystem::EndFrame(ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* cbvHeap,
    ID3D12DescriptorHeap* samplerHeap,
    UINT descriptorSize,
    GBuffer* gBuffer,
    D3D12_GPU_VIRTUAL_ADDRESS passCBAddress,
    D3D12_GPU_VIRTUAL_ADDRESS lightBufferAddress,
    UINT numLights)
{
    gBuffer->TransitionToShaderResource(cmdList);

    // Биндим обе heap
    ID3D12DescriptorHeap* heaps[] = { cbvHeap, samplerHeap };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    cmdList->SetGraphicsRootSignature(mDeferredRootSig.Get());
    cmdList->SetPipelineState(mDeferredPSO.Get());

    // Слот 0: G-Buffer SRV
    CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(
        cbvHeap->GetGPUDescriptorHandleForHeapStart());
    srvHandle.Offset(2, descriptorSize);
    cmdList->SetGraphicsRootDescriptorTable(0, srvHandle);

    cmdList->SetGraphicsRootConstantBufferView(1, passCBAddress);

    cmdList->SetGraphicsRootShaderResourceView(2, lightBufferAddress);

    // Слот 2: Sampler
    cmdList->SetGraphicsRootDescriptorTable(3,
        samplerHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->RSSetViewports(1, &mViewport);
    cmdList->RSSetScissorRects(1, &mScissor);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::BuildGeometryPSO()
{
    auto vs = d3dUtil::CompileShader(L"Shaders\\geometry_vs.hlsl", nullptr, "main", "vs_5_0");
    auto ps = d3dUtil::CompileShader(L"Shaders\\geometry_ps.hlsl", nullptr, "main", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
    ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

    // Input Layout для Geometry Pass
    std::vector<D3D12_INPUT_ELEMENT_DESC> geoInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    psoDesc.InputLayout = { geoInputLayout.data(), (UINT)geoInputLayout.size() };
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.SampleDesc.Count = mMsaaState ? 4 : 1;
    psoDesc.SampleDesc.Quality = mMsaaState ? (mMsaaQuality - 1) : 0;

    psoDesc.NumRenderTargets = 3;
    psoDesc.RTVFormats[0] = mFormats[0];
    psoDesc.RTVFormats[1] = mFormats[1];
    psoDesc.RTVFormats[2] = mFormats[2];
    psoDesc.DSVFormat = mDepthStencilFormat;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mGeometryPSO)));
}

void RenderingSystem::BuildTessellationPSO()
{
    auto vs = d3dUtil::CompileShader(L"Shaders\\tessellation_vs.hlsl", nullptr, "main", "vs_5_0");
    auto hs = d3dUtil::CompileShader(L"Shaders\\tessellation_hs.hlsl", nullptr, "main", "hs_5_0");
    auto ds = d3dUtil::CompileShader(L"Shaders\\tessellation_ds.hlsl", nullptr, "main", "ds_5_0");
    auto ps = d3dUtil::CompileShader(L"Shaders\\tessellation_ps.hlsl", nullptr, "main", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

    // ============================================
    // 1. ROOT SIGNATURE
    // ============================================
    psoDesc.pRootSignature = mTessellationRootSig.Get();

    // ============================================
    // 2. ШЕЙДЕРЫ
    // ============================================
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.HS = { hs->GetBufferPointer(), hs->GetBufferSize() };
    psoDesc.DS = { ds->GetBufferPointer(), ds->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };

    // ============================================
    // 3. INPUT LAYOUT (для вершинного шейдера)
    // ============================================
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    psoDesc.InputLayout = { inputLayout.data(), (UINT)inputLayout.size() };

    // ============================================
    // 4. ПРИМИТИВЫ — ВАЖНО: PATCH!
    // ============================================
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;

    // ============================================
    // 5. RASTERIZER STATE
    // ============================================
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;

    // ============================================
    // 6. BLEND STATE
    // ============================================
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // ============================================
    // 7. DEPTH STENCIL STATE
    // ============================================
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    // ============================================
    // 8. SAMPLE MASK
    // ============================================
    psoDesc.SampleMask = UINT_MAX;

    // ============================================
    // 9. RENDER TARGET
    // ============================================
    psoDesc.NumRenderTargets = 3;
    psoDesc.RTVFormats[0] = mFormats[0];
    psoDesc.RTVFormats[1] = mFormats[1];
    psoDesc.RTVFormats[2] = mFormats[2];
    psoDesc.DSVFormat = mDepthStencilFormat;

    // ============================================
    // 10. MULTISAMPLING
    // ============================================
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mTessellationPSO)));
}

void RenderingSystem::BuildTessellationRootSignature()
{
    // ============================================
    // 3 СЛОТА: CBV + SRV + Sampler
    // ============================================
    CD3DX12_ROOT_PARAMETER slotRootParameter[3];

    // Слот 0: Константы тесселяции (CBV)
    slotRootParameter[0].InitAsConstantBufferView(0);  // b0

    // Слот 1: Текстуры (Descriptor Table) — t0, t1, t2
    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);  // t0, t1, t2
    slotRootParameter[1].InitAsDescriptorTable(1, &texTable);

    // ============================================
    // Слот 2: СЕМПЛЕР (s0)
    // ============================================
    CD3DX12_DESCRIPTOR_RANGE samplerTable;
    samplerTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);  // s0
    slotRootParameter[2].InitAsDescriptorTable(1, &samplerTable);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        3,                      // ← ТЕПЕРЬ 3!
        slotRootParameter,
        0,
        nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
    );

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(
        &rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(),
        errorBlob.GetAddressOf()
    );

    if (errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mTessellationRootSig)
    ));
}