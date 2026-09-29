#pragma once

#include "../framework/d3dUtil.h"
#include "../framework/UploadBuffer.h"

// ============================================================
// Пост-обработка
//   * lighting pass рисует в промежуточную текстуру SceneColor
//   * Bloom: Bright pass (SceneColor -> 1/4 разрешения) + размытие Гаусса H/V (пинг-понг A <-> B)
//   * финальный проход (full-screen треугольник без VB): SceneColor + Bloom + G-Buffer -> back buffer
//     эффекты: хроматическая аберрация, bloom
// ============================================================

// Совпадает с cbPost (b0) в post_common.hlsli
struct PostConstants
{
    DirectX::XMFLOAT3 EyePosW = { 0, 0, 0 };
    float DebugDepthRange = 5000.0f;

    float BloomThreshold = 0.6f;     // с какой яркости (0..1) начинается свечение
    float BloomKnee = 0.2f;          // мягкость порога
    float BloomIntensity = 1.0f;     // сила свечения при сложении
    float ChromaticStrength = 0.006f;// смещение каналов на краю экрана (в долях UV)

    int DebugView = 0;               // 0 итог, 1 альбедо, 2 нормали, 3 расстояние, 4 только bloom
    int BloomEnabled = 1;
    int ChromaticEnabled = 1;
    int pad0 = 0;
};

class PostProcess
{
public:
    static constexpr UINT SrvCount = 3;      // SceneColor, BloomA, BloomB
    static constexpr UINT BlurIterations = 2; // сколько раз H+V размытие

    PostProcess(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT sceneFormat)
        : md3dDevice(device), mWidth(width), mHeight(height), mSceneFormat(sceneFormat)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeap = {};
        rtvHeap.NumDescriptors = 3;          // 0 SceneColor, 1 BloomA, 2 BloomB
        rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&mRtvHeap)));
        mRtvSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        BuildRootSignature();
        BuildPSOs();
        mConstants = std::make_unique<UploadBuffer<PostConstants>>(md3dDevice, 1, true);
    }

    // 3 подряд идущих SRV в общей shader-visible куче: [SceneColor, BloomA, BloomB]
    void BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpu, CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpu, UINT srvSize)
    {
        mSrvCpu = srvCpu;
        mSrvGpu = srvGpu;
        mSrvSize = srvSize;
        BuildResources();
    }

    void OnResize(UINT width, UINT height)
    {
        if (width == mWidth && height == mHeight) return;
        mWidth = width;
        mHeight = height;
        BuildResources();
    }

    void SetConstants(const PostConstants& c) { mConstants->CopyData(0, c); mBloomEnabled = c.BloomEnabled != 0 || c.DebugView == 4; }

    void TransitionToRenderTarget(ID3D12GraphicsCommandList* cmd) { Transition(cmd, mSceneColor.Get(), true); }
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmd) { Transition(cmd, mSceneColor.Get(), false); }

    D3D12_CPU_DESCRIPTOR_HANDLE SceneColorRtv() const { return Rtv(0); }

    // Вся пост-обработка: bloom-проходы, затем финальный проход в backBufferRtv
    void Execute(ID3D12GraphicsCommandList* cmd,
        ID3D12DescriptorHeap* cbvSrvHeap,
        D3D12_GPU_DESCRIPTOR_HANDLE gBufferSrvTable,   // t0..t2: albedo, normal, position
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissor)
    {
        ID3D12DescriptorHeap* heaps[] = { cbvSrvHeap };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootSignature(mRootSig.Get());
        cmd->SetGraphicsRootDescriptorTable(0, gBufferSrvTable);
        cmd->SetGraphicsRootConstantBufferView(2, mConstants->Resource()->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // ================= BLOOM =================
        if (mBloomEnabled)
        {
            cmd->RSSetViewports(1, &mBloomViewport);
            cmd->RSSetScissorRects(1, &mBloomScissor);

            // 1) Bright pass: яркие пиксели SceneColor -> BloomA (1/4 разрешения)
            RunPass(cmd, mBrightPSO.Get(), Srv(0), mBloomA.Get(), Rtv(1), 0.0f, 0.0f);

            // 2) Размытие Гаусса, раздельно по осям: A -> B (по X), B -> A (по Y)
            for (UINT i = 0; i < BlurIterations; ++i)
            {
                RunPass(cmd, mBlurPSO.Get(), Srv(1), mBloomB.Get(), Rtv(2), 1.0f, 0.0f);
                RunPass(cmd, mBlurPSO.Get(), Srv(2), mBloomA.Get(), Rtv(1), 0.0f, 1.0f);
            }
        }

        // ================= ФИНАЛЬНЫЙ ПРОХОД =================
        cmd->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);

        cmd->SetPipelineState(mFinalPSO.Get());
        cmd->SetGraphicsRootDescriptorTable(1, Srv(0));   // t3: SceneColor
        cmd->SetGraphicsRootDescriptorTable(3, Srv(1));   // t4: размытый bloom (BloomA)

        // Ни вершинного, ни индексного буфера: координаты строит VS из SV_VertexID
        cmd->DrawInstanced(3, 1, 0, 0);
    }

private:
    // Один полноэкранный проход: input SRV -> target (с переходами состояний)
    void RunPass(ID3D12GraphicsCommandList* cmd, ID3D12PipelineState* pso,
        D3D12_GPU_DESCRIPTOR_HANDLE inputSrv, ID3D12Resource* target,
        D3D12_CPU_DESCRIPTOR_HANDLE targetRtv, float dirX, float dirY)
    {
        Transition(cmd, target, true);
        cmd->OMSetRenderTargets(1, &targetRtv, TRUE, nullptr);

        cmd->SetPipelineState(pso);
        cmd->SetGraphicsRootDescriptorTable(1, inputSrv);

        // b1: направление размытия и размер текселя bloom-текстуры
        const float rc[4] = { dirX, dirY, 1.0f / (float)mBloomWidth, 1.0f / (float)mBloomHeight };
        cmd->SetGraphicsRoot32BitConstants(4, 4, rc, 0);

        cmd->DrawInstanced(3, 1, 0, 0);
        Transition(cmd, target, false);
    }

    static void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, bool toRenderTarget)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(r,
            toRenderTarget ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET,
            toRenderTarget ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &b);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT i) const
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart(), (INT)i, mRtvSize);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Srv(UINT i) const
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvGpu, (INT)i, mSrvSize);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateTarget(UINT w, UINT h, DXGI_FORMAT format, UINT index)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = format;
        clear.Color[3] = 1.0f;

        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(md3dDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&res)));

        md3dDevice->CreateRenderTargetView(res.Get(), nullptr, Rtv(index));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        md3dDevice->CreateShaderResourceView(res.Get(), &srv,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvCpu, (INT)index, mSrvSize));
        return res;
    }

    void BuildResources()
    {
        mBloomWidth = (std::max)(1u, mWidth / 4);
        mBloomHeight = (std::max)(1u, mHeight / 4);
        mBloomViewport = { 0.0f, 0.0f, (float)mBloomWidth, (float)mBloomHeight, 0.0f, 1.0f };
        mBloomScissor = { 0, 0, (LONG)mBloomWidth, (LONG)mBloomHeight };

        mSceneColor = CreateTarget(mWidth, mHeight, mSceneFormat, 0);
        mBloomA = CreateTarget(mBloomWidth, mBloomHeight, kBloomFormat, 1);
        mBloomB = CreateTarget(mBloomWidth, mBloomHeight, kBloomFormat, 2);
    }

    void BuildRootSignature()
    {
        CD3DX12_DESCRIPTOR_RANGE gBufferRange;
        gBufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);   // t0..t2 G-Buffer
        CD3DX12_DESCRIPTOR_RANGE inputRange;
        inputRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);     // t3 входная текстура прохода
        CD3DX12_DESCRIPTOR_RANGE bloomRange;
        bloomRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);     // t4 bloom (для финального прохода)

        CD3DX12_ROOT_PARAMETER params[5];
        params[0].InitAsDescriptorTable(1, &gBufferRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &inputRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);   // b0
        params[3].InitAsDescriptorTable(1, &bloomRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[4].InitAsConstants(4, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);        // b1: 4 float

        CD3DX12_STATIC_SAMPLER_DESC samplers[2] =
        {
            CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
            CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP)
        };

        CD3DX12_ROOT_SIGNATURE_DESC desc(5, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
            blob.GetAddressOf(), error.GetAddressOf());
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        ThrowIfFailed(hr);
        ThrowIfFailed(md3dDevice->CreateRootSignature(0, blob->GetBufferPointer(),
            blob->GetBufferSize(), IID_PPV_ARGS(&mRootSig)));
    }

    Microsoft::WRL::ComPtr<ID3D12PipelineState> MakePSO(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtvFormat)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.InputLayout = { nullptr, 0 };                 // вершин нет
        pso.pRootSignature = mRootSig.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = rtvFormat;
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc.Count = 1;

        Microsoft::WRL::ComPtr<ID3D12PipelineState> result;
        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&result)));
        return result;
    }

    void BuildPSOs()
    {
        auto vs = d3dUtil::CompileShader(L"Shaders\\fullscreen_vs.hlsl", nullptr, "main", "vs_5_0");
        auto finalPS = d3dUtil::CompileShader(L"Shaders\\postprocess_ps.hlsl", nullptr, "main", "ps_5_0");
        auto brightPS = d3dUtil::CompileShader(L"Shaders\\bloom_ps.hlsl", nullptr, "Bright", "ps_5_0");
        auto blurPS = d3dUtil::CompileShader(L"Shaders\\bloom_ps.hlsl", nullptr, "Blur", "ps_5_0");

        mFinalPSO = MakePSO(vs.Get(), finalPS.Get(), mSceneFormat);
        mBrightPSO = MakePSO(vs.Get(), brightPS.Get(), kBloomFormat);
        mBlurPSO = MakePSO(vs.Get(), blurPS.Get(), kBloomFormat);
    }

    // Размытие в 16-битном float: без полос при многократном размытии
    static constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    ID3D12Device* md3dDevice = nullptr;
    UINT mWidth = 0, mHeight = 0;
    UINT mBloomWidth = 1, mBloomHeight = 1;
    DXGI_FORMAT mSceneFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool mBloomEnabled = true;

    Microsoft::WRL::ComPtr<ID3D12Resource> mSceneColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBloomA;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBloomB;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    UINT mRtvSize = 0;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mSrvCpu;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mSrvGpu;
    UINT mSrvSize = 0;

    D3D12_VIEWPORT mBloomViewport = {};
    D3D12_RECT mBloomScissor = {};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mFinalPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mBrightPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mBlurPSO;
    std::unique_ptr<UploadBuffer<PostConstants>> mConstants;
};