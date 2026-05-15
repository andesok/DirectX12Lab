#include "../framework/RenderingSystem.h"

void RenderingSystem::Initialize() {
    BuildRootSignature();
    BuildShadersAndInputLayout();
    BuildPSO();
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
    // Shader programs typically require resources as input (constant buffers,
    // textures, samplers).  The root signature defines the resources the shader
    // programs expect.  If we think of the shader programs as a function, and
    // the input resources as function parameters, then the root signature can be
    // thought of as defining the function signature.  

    // Root parameter can be a table, root descriptor or root constants.
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

    // create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
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
    HRESULT hr = S_OK;

    mvsByteCode = d3dUtil::CompileShader(L"Shaders\\texture_vs.hlsl", nullptr, "VS", "vs_5_0");
    mpsByteCode = d3dUtil::CompileShader(L"Shaders\\texture_ps.hlsl", nullptr, "PS", "ps_5_0");

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
    psoDesc.RTVFormats[0] = mFormats[0]; // Albedo
    //psoDesc.RTVFormats[1] = mFormats[1]; // Normal
    //psoDesc.RTVFormats[2] = mFormats[2]; // Position
    psoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));
}

void RenderingSystem::BeginFrame(ID3D12GraphicsCommandList* cmdList, D3D12_VIEWPORT& vp, D3D12_RECT& sc) {
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);
    cmdList->SetGraphicsRootSignature(mRootSignature.Get());
    cmdList->SetPipelineState(mPSO.Get());
}

void RenderingSystem::DrawItem(ID3D12GraphicsCommandList* cmdList,
    const RenderItem& item,
    ID3D12DescriptorHeap* mainHeap,
    ID3D12DescriptorHeap* samplerHeap)
{
    // 1. Устанавливаем кучи (CBV_SRV_UAV и Sampler)
    ID3D12DescriptorHeap* heaps[] = { mainHeap, samplerHeap };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    // 2. Рассчитываем смещения в куче для текущего объекта
    auto handle = mainHeap->GetGPUDescriptorHandleForHeapStart();
    UINT descriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Привязываем CBV (константы: World, Time, Tiling)
    auto cbvHandle = handle;
    cbvHandle.ptr += item.CBIndex * descriptorSize;
    cmdList->SetGraphicsRootDescriptorTable(0, cbvHandle);

    // Привязываем SRV (Текстура материала из OBJ)
    auto srvHandle = handle;
    srvHandle.ptr += item.SRVIndex * descriptorSize;
    cmdList->SetGraphicsRootDescriptorTable(1, srvHandle);

    // 3. Привязываем Sampler (всегда в начале своей кучи)
    cmdList->SetGraphicsRootDescriptorTable(2, samplerHeap->GetGPUDescriptorHandleForHeapStart());

    // 4. Отрисовка геометрии
    auto vbv = item.Mesh->VertexBufferView();
    auto ibv = item.Mesh->IndexBufferView();
    cmdList->IASetVertexBuffers(0, 1, &vbv);
    cmdList->IASetIndexBuffer(&ibv);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    auto& submesh = item.Mesh->DrawArgs[item.SubmeshName];
    cmdList->DrawIndexedInstanced(submesh.IndexCount, 1, 0, 0, 0);
}