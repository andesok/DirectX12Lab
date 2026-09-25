#pragma once

#include "../framework/d3dUtil.h"
#include "../framework/UploadBuffer.h"
#include <cmath>

// ============================================================
// GPU-система частиц (ДЗ №6)
//   * два буфера частиц: Consume (текущие) -> Append (выжившие + новые), пинг-понг
//   * эмиссия и обновление — только в Compute-шейдерах
//   * отрисовка: VS читает StructuredBuffer, GS разворачивает точку в билборд,
//     PS пишет непрозрачную частицу в G-Buffer
//   * число вершин для отрисовки GPU берёт сам (ExecuteIndirect из счётчика Append)
// ============================================================

// Совпадает со struct Particle в particles_common.hlsli (48 байт)
struct GpuParticle
{
    DirectX::XMFLOAT3 Position;
    float Age;
    DirectX::XMFLOAT3 Velocity;
    float Lifetime;
    DirectX::XMFLOAT3 Color;
    float Size;
};
static_assert(sizeof(GpuParticle) == 48, "GpuParticle must match HLSL layout");

// Совпадает с cbSim (b0) в particles_cs.hlsl
struct ParticleSimConstants
{
    DirectX::XMFLOAT3 EmitterPos = { 0, 0, 0 };
    float DeltaTime = 0.0f;
    DirectX::XMFLOAT3 Gravity = { 0, -980.0f, 0 };
    float TotalTime = 0.0f;
    float FloorY = 0.0f;
    float Bounce = 0.35f;
    UINT EmitCount = 0;
    UINT MaxParticles = 0;
    float SpeedMin = 0.0f;
    float SpeedMax = 0.0f;
    float ConeAngle = 0.0f;
    float EmitterRadius = 0.0f;
    float LifetimeMin = 0.0f;
    float LifetimeMax = 0.0f;
    float SizeMin = 0.0f;
    float SizeMax = 0.0f;
};

// Совпадает с cbParticleDraw (b0) в particles_draw.hlsl
struct ParticleDrawConstants
{
    DirectX::XMFLOAT4X4 ViewProj;
    DirectX::XMFLOAT3 CameraRight; float pad0;
    DirectX::XMFLOAT3 CameraUp;    float pad1;
    DirectX::XMFLOAT3 CameraLook;  float pad2;
};

// Параметры фонтана, задаются из App
struct FountainSettings
{
    DirectX::XMFLOAT3 Position = { 0, 0, 0 };
    float FloorY = 0.0f;
    float EmitRate = 20000.0f;       // частиц в секунду
    float SpeedMin = 1000.0f;
    float SpeedMax = 1200.0f;
    float ConeAngle = 0.25f;         // радианы от вертикали
    float EmitterRadius = 5.0f;
    float LifetimeMin = 3.0f;
    float LifetimeMax = 5.0f;
    float SizeMin = 3.0f;
    float SizeMax = 6.0f;
    float Gravity = 980.0f;
    float Bounce = 0.35f;
    bool Enabled = true;
};

class ParticleSystem
{
public:
    static constexpr UINT DescriptorCount = 4;   // UAV: [P0, P1, P1, P0]

    ParticleSystem(ID3D12Device* device, UINT maxParticles,
        const DXGI_FORMAT gBufferFormats[3], DXGI_FORMAT depthFormat)
        : md3dDevice(device), mMaxParticles(maxParticles), mDepthFormat(depthFormat)
    {
        for (int i = 0; i < 3; ++i) mGBufferFormats[i] = gBufferFormats[i];
    }

    // Вызывать при открытом command list (записываются начальные копирования)
    void Initialize(ID3D12GraphicsCommandList* cmdList)
    {
        BuildBuffers(cmdList);
        BuildComputeRootSignature();
        BuildComputePSOs();
        BuildDrawRootSignature();
        BuildDrawPSO();
        BuildCommandSignature();

        mSimCB = std::make_unique<UploadBuffer<ParticleSimConstants>>(md3dDevice, 1, true);
        mDrawCB = std::make_unique<UploadBuffer<ParticleDrawConstants>>(md3dDevice, 1, true);
    }

    // 4 UAV-дескриптора в shader-visible куче. Порядок [P0,P1,P1,P0]:
    // таблица u0..u1 с начала = (Consume P0, Append P1), со смещения 2 = (Consume P1, Append P0)
    void BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE gpu, UINT descriptorSize)
    {
        mUavGpuBase = gpu;
        mDescriptorSize = descriptorSize;

        const int order[DescriptorCount] = { 0, 1, 1, 0 };
        for (UINT i = 0; i < DescriptorCount; ++i)
        {
            const int b = order[i];

            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.FirstElement = 0;
            uav.Buffer.NumElements = mMaxParticles;
            uav.Buffer.StructureByteStride = sizeof(GpuParticle);
            uav.Buffer.CounterOffsetInBytes = 0;          // счётчик — в отдельном ресурсе
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

            CD3DX12_CPU_DESCRIPTOR_HANDLE h(cpu, (INT)i, descriptorSize);
            md3dDevice->CreateUnorderedAccessView(mParticles[b].Get(), mCounters[b].Get(), &uav, h);
        }
    }

    // CPU-часть кадра: константы симуляции и камеры
    void Update(float dt, float totalTime, const FountainSettings& s,
        DirectX::FXMMATRIX viewProj, const DirectX::XMFLOAT3& camRight,
        const DirectX::XMFLOAT3& camUp, const DirectX::XMFLOAT3& camLook)
    {
        using namespace DirectX;

        // Дробное накопление, чтобы при высоком fps эмиссия не обнулялась
        UINT emitCount = 0;
        if (s.Enabled)
        {
            mEmitAccumulator += s.EmitRate * dt;
            const float whole = std::floor(mEmitAccumulator);
            mEmitAccumulator -= whole;
            emitCount = (UINT)(std::min)(whole, (float)mMaxParticles);
        }
        else
        {
            mEmitAccumulator = 0.0f;
        }
        mLastEmitCount = emitCount;

        ParticleSimConstants sim;
        sim.EmitterPos = s.Position;
        sim.DeltaTime = (std::min)(dt, 0.05f);   // защита от скачка dt после паузы
        sim.Gravity = { 0.0f, -s.Gravity, 0.0f };
        sim.TotalTime = totalTime;
        sim.FloorY = s.FloorY;
        sim.Bounce = s.Bounce;
        sim.EmitCount = emitCount;
        sim.MaxParticles = mMaxParticles;
        sim.SpeedMin = s.SpeedMin;
        sim.SpeedMax = s.SpeedMax;
        sim.ConeAngle = s.ConeAngle;
        sim.EmitterRadius = s.EmitterRadius;
        sim.LifetimeMin = s.LifetimeMin;
        sim.LifetimeMax = s.LifetimeMax;
        sim.SizeMin = s.SizeMin;
        sim.SizeMax = s.SizeMax;
        mSimCB->CopyData(0, sim);

        ParticleDrawConstants draw;
        XMStoreFloat4x4(&draw.ViewProj, XMMatrixTranspose(viewProj));
        draw.CameraRight = camRight; draw.pad0 = 0.0f;
        draw.CameraUp = camUp;       draw.pad1 = 0.0f;
        draw.CameraLook = camLook;   draw.pad2 = 0.0f;
        mDrawCB->CopyData(0, draw);
    }

    // GPU: Update (Consume -> Append) + Emit (Append), затем счётчик -> аргументы отрисовки
    void Simulate(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* cbvSrvUavHeap)
    {
        const UINT src = mCurrent;       // Consume
        const UINT dst = 1 - mCurrent;   // Append

        // ---- 1. Обнуляем счётчик приёмника, копируем число живых в константный буфер ----
        {
            D3D12_RESOURCE_BARRIER b[3] = {
                Transition(mCounters[dst].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                Transition(mCounters[src].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(mAliveCountCB.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
            cmd->ResourceBarrier(3, b);
        }
        cmd->CopyBufferRegion(mCounters[dst].Get(), 0, mZeroUpload.Get(), 0, sizeof(UINT));
        cmd->CopyBufferRegion(mAliveCountCB.Get(), 0, mCounters[src].Get(), 0, sizeof(UINT));
        {
            D3D12_RESOURCE_BARRIER b[5] = {
                Transition(mCounters[dst].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Transition(mCounters[src].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Transition(mAliveCountCB.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
                Transition(mParticles[src].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Transition(mParticles[dst].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) };
            cmd->ResourceBarrier(5, b);
        }

        // ---- 2. Общие привязки compute ----
        ID3D12DescriptorHeap* heaps[] = { cbvSrvUavHeap };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(mComputeRootSig.Get());
        cmd->SetComputeRootConstantBufferView(0, mSimCB->Resource()->GetGPUVirtualAddress());
        cmd->SetComputeRootConstantBufferView(1, mAliveCountCB->GetGPUVirtualAddress());

        // Счётчики Append/Consume работают ТОЛЬКО через descriptor table
        CD3DX12_GPU_DESCRIPTOR_HANDLE table(mUavGpuBase, src == 0 ? 0 : 2, mDescriptorSize);
        cmd->SetComputeRootDescriptorTable(2, table);

        // ---- 3. Update: каждый поток с id < aliveCount делает один Consume ----
        cmd->SetPipelineState(mUpdatePSO.Get());
        cmd->Dispatch((mMaxParticles + 255) / 256, 1, 1);

        // Emit тоже пишет в dst — ждём окончания Update
        {
            // nullptr = барьер для всех UAV (и буфера частиц, и его счётчика)
            auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
            cmd->ResourceBarrier(1, &uavBarrier);
        }

        // ---- 4. Emit: новые частицы Append в dst ----
        if (mLastEmitCount > 0)
        {
            cmd->SetPipelineState(mEmitPSO.Get());
            cmd->Dispatch((mLastEmitCount + 63) / 64, 1, 1);
        }

        // ---- 5. Счётчик dst -> аргументы ExecuteIndirect (+ копия для вывода на CPU) ----
        {
            D3D12_RESOURCE_BARRIER b[6] = {
                Transition(mParticles[dst].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                Transition(mCounters[dst].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(mDrawArgs.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                Transition(mParticles[src].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
                Transition(mCounters[src].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
                Transition(mAliveCountCB.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COMMON) };
            cmd->ResourceBarrier(6, b);
        }
        // D3D12_DRAW_ARGUMENTS::VertexCountPerInstance — первые 4 байта
        cmd->CopyBufferRegion(mDrawArgs.Get(), 0, mCounters[dst].Get(), 0, sizeof(UINT));
        cmd->CopyBufferRegion(mReadback.Get(), 0, mCounters[dst].Get(), 0, sizeof(UINT));
        {
            D3D12_RESOURCE_BARRIER b[2] = {
                Transition(mDrawArgs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
                Transition(mCounters[dst].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON) };
            cmd->ResourceBarrier(2, b);
        }
    }

    // Отрисовка в G-Buffer (вызывать внутри geometry pass: RTV G-Buffer + DSV уже установлены)
    void Draw(ID3D12GraphicsCommandList* cmd)
    {
        const UINT dst = 1 - mCurrent;

        cmd->SetPipelineState(mDrawPSO.Get());
        cmd->SetGraphicsRootSignature(mDrawRootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(0, mDrawCB->Resource()->GetGPUVirtualAddress());
        cmd->SetGraphicsRootShaderResourceView(1, mParticles[dst]->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

        // Число вершин (= живых частиц) GPU берёт из mDrawArgs — CPU его не знает
        cmd->ExecuteIndirect(mCommandSignature.Get(), 1, mDrawArgs.Get(), 0, nullptr, 0);
    }

    // Возвращаем ресурсы в COMMON и меняем буферы местами
    void EndFrame(ID3D12GraphicsCommandList* cmd)
    {
        const UINT dst = 1 - mCurrent;
        D3D12_RESOURCE_BARRIER b[2] = {
            Transition(mParticles[dst].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            Transition(mDrawArgs.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON) };
        cmd->ResourceBarrier(2, b);

        mCurrent = dst;   // выжившие стали входом следующего кадра
    }

    // Число живых частиц после прошлого кадра (Draw() делает Flush, так что данные готовы)
    UINT ReadAliveCount()
    {
        UINT value = 0;
        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, sizeof(UINT) };
        if (SUCCEEDED(mReadback->Map(0, &readRange, &mapped)))
        {
            value = *reinterpret_cast<UINT*>(mapped);
            D3D12_RANGE writeRange = { 0, 0 };
            mReadback->Unmap(0, &writeRange);
        }
        return value;
    }

    UINT MaxParticles() const { return mMaxParticles; }
    UINT LastEmitCount() const { return mLastEmitCount; }

private:
    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        return CD3DX12_RESOURCE_BARRIER::Transition(r, before, after);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        CD3DX12_HEAP_PROPERTIES heap(heapType);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
        ThrowIfFailed(md3dDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            state, nullptr, IID_PPV_ARGS(&res)));
        return res;
    }

    void BuildBuffers(ID3D12GraphicsCommandList* cmd)
    {
        const UINT64 particleBytes = (UINT64)mMaxParticles * sizeof(GpuParticle);

        for (int i = 0; i < 2; ++i)
        {
            mParticles[i] = CreateBuffer(particleBytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            // Счётчик Append/Consume — отдельный 4-байтный ресурс
            mCounters[i] = CreateBuffer(sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }

        // Число живых для шейдера (root CBV => 256 байт)
        mAliveCountCB = CreateBuffer(256, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);

        // Аргументы DrawInstanced: {VertexCount, InstanceCount=1, StartVertex=0, StartInstance=0}
        mDrawArgs = CreateBuffer(sizeof(D3D12_DRAW_ARGUMENTS), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);

        // Upload: 0 для сброса счётчика + начальные аргументы
        mZeroUpload = CreateBuffer(256, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        {
            BYTE* p = nullptr;
            ThrowIfFailed(mZeroUpload->Map(0, nullptr, reinterpret_cast<void**>(&p)));
            ZeroMemory(p, 256);
            D3D12_DRAW_ARGUMENTS args = { 0, 1, 0, 0 };
            memcpy(p + 16, &args, sizeof(args));   // смещение 16: начальные аргументы
            mZeroUpload->Unmap(0, nullptr);
        }

        mReadback = CreateBuffer(sizeof(UINT), D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        // Начальное состояние: счётчики = 0, аргументы = {0,1,0,0}
        {
            D3D12_RESOURCE_BARRIER b[3] = {
                Transition(mCounters[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                Transition(mCounters[1].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                Transition(mDrawArgs.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
            cmd->ResourceBarrier(3, b);
        }
        cmd->CopyBufferRegion(mCounters[0].Get(), 0, mZeroUpload.Get(), 0, sizeof(UINT));
        cmd->CopyBufferRegion(mCounters[1].Get(), 0, mZeroUpload.Get(), 0, sizeof(UINT));
        cmd->CopyBufferRegion(mDrawArgs.Get(), 0, mZeroUpload.Get(), 16, sizeof(D3D12_DRAW_ARGUMENTS));
        {
            D3D12_RESOURCE_BARRIER b[3] = {
                Transition(mCounters[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
                Transition(mCounters[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
                Transition(mDrawArgs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON) };
            cmd->ResourceBarrier(3, b);
        }
    }

    static void SerializeAndCreate(ID3D12Device* device, const CD3DX12_ROOT_SIGNATURE_DESC& desc,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& out)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
            blob.GetAddressOf(), error.GetAddressOf());
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        ThrowIfFailed(hr);
        ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&out)));
    }

    void BuildComputeRootSignature()
    {
        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0);   // b0: параметры симуляции
        params[1].InitAsConstantBufferView(1);   // b1: число живых частиц

        CD3DX12_DESCRIPTOR_RANGE uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);   // u0 Consume, u1 Append
        params[2].InitAsDescriptorTable(1, &uavRange);

        CD3DX12_ROOT_SIGNATURE_DESC desc(3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        SerializeAndCreate(md3dDevice, desc, mComputeRootSig);
    }

    void BuildComputePSOs()
    {
        auto emitCS = d3dUtil::CompileShader(L"Shaders\\particles_cs.hlsl", nullptr, "Emit", "cs_5_0");
        auto updateCS = d3dUtil::CompileShader(L"Shaders\\particles_cs.hlsl", nullptr, "Update", "cs_5_0");

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = mComputeRootSig.Get();

        pso.CS = { emitCS->GetBufferPointer(), emitCS->GetBufferSize() };
        ThrowIfFailed(md3dDevice->CreateComputePipelineState(&pso, IID_PPV_ARGS(&mEmitPSO)));

        pso.CS = { updateCS->GetBufferPointer(), updateCS->GetBufferSize() };
        ThrowIfFailed(md3dDevice->CreateComputePipelineState(&pso, IID_PPV_ARGS(&mUpdatePSO)));
    }

    void BuildDrawRootSignature()
    {
        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0);   // b0: камера
        params[1].InitAsShaderResourceView(0);   // t0: StructuredBuffer<Particle> (root SRV)

        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        SerializeAndCreate(md3dDevice, desc, mDrawRootSig);
    }

    void BuildDrawPSO()
    {
        auto vs = d3dUtil::CompileShader(L"Shaders\\particles_draw.hlsl", nullptr, "VS", "vs_5_0");
        auto gs = d3dUtil::CompileShader(L"Shaders\\particles_draw.hlsl", nullptr, "GS", "gs_5_0");
        auto ps = d3dUtil::CompileShader(L"Shaders\\particles_draw.hlsl", nullptr, "PS", "ps_5_0");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.InputLayout = { nullptr, 0 };         // вершин нет — VS читает буфер по SV_VertexID
        pso.pRootSignature = mDrawRootSig.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.GS = { gs->GetBufferPointer(), gs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);          // непрозрачные: без смешивания
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        pso.NumRenderTargets = 3;
        pso.RTVFormats[0] = mGBufferFormats[0];
        pso.RTVFormats[1] = mGBufferFormats[1];
        pso.RTVFormats[2] = mGBufferFormats[2];
        pso.DSVFormat = mDepthFormat;
        pso.SampleDesc.Count = 1;
        pso.SampleDesc.Quality = 0;
        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&mDrawPSO)));
    }

    void BuildCommandSignature()
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg = {};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

        D3D12_COMMAND_SIGNATURE_DESC desc = {};
        desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &arg;
        // Только аргументы Draw — root signature не нужна
        ThrowIfFailed(md3dDevice->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&mCommandSignature)));
    }

    ID3D12Device* md3dDevice = nullptr;
    UINT mMaxParticles = 0;
    DXGI_FORMAT mGBufferFormats[3] = {};
    DXGI_FORMAT mDepthFormat = DXGI_FORMAT_UNKNOWN;

    UINT mCurrent = 0;   // индекс буфера, из которого в этом кадре делается Consume

    Microsoft::WRL::ComPtr<ID3D12Resource> mParticles[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> mCounters[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> mAliveCountCB;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDrawArgs;
    Microsoft::WRL::ComPtr<ID3D12Resource> mZeroUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> mReadback;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mEmitPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mUpdatePSO;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mDrawRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDrawPSO;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mCommandSignature;

    std::unique_ptr<UploadBuffer<ParticleSimConstants>> mSimCB;
    std::unique_ptr<UploadBuffer<ParticleDrawConstants>> mDrawCB;

    D3D12_GPU_DESCRIPTOR_HANDLE mUavGpuBase = {};
    UINT mDescriptorSize = 0;

    float mEmitAccumulator = 0.0f;
    UINT mLastEmitCount = 0;
};