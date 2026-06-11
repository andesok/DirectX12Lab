#pragma once

#include "../framework/d3dApp.h"
#include "../framework/MathHelper.h"
#include "../framework/UploadBuffer.h"
#include "../framework/GBuffer.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct RenderConstants {
    XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
    float TotalTime = 0.0f;
    XMFLOAT2 Tiling = { 1.0f, 1.0f };
    float Padding;
};

enum class ShaderType {
    TEXTURE,
    WAVE
};

struct RenderItem {
    MeshGeometry* Mesh = nullptr;
    std::string SubmeshName;
    UINT CBIndex = 0;
    UINT SRVIndex = 0;
    ShaderType Shader = ShaderType::TEXTURE;
};

class RenderingSystem {
public:
    RenderingSystem(ID3D12Device* device, const DXGI_FORMAT gBufferFormats[3], DXGI_FORMAT depthBufferFormat, bool msaaState, UINT msaaQuality);
    ~RenderingSystem();

    void Initialize();
    void RenderingSystem::BeginFrame(ID3D12GraphicsCommandList* cmdList,
        D3D12_VIEWPORT& viewport,
        D3D12_RECT& scissor,
        GBuffer* gBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv);
    void DrawItem(ID3D12GraphicsCommandList* cmdList,
        const RenderItem& item,
        ID3D12DescriptorHeap* mainHeap,
        ID3D12DescriptorHeap* samplerHeap);
    void SetGBuffer(GBuffer* gBuffer) { mGBuffer = gBuffer; }
    void RenderingSystem::EndFrame(ID3D12GraphicsCommandList* cmdList,
        ID3D12DescriptorHeap* cbvHeap,
        UINT descriptorSize,
        GBuffer* gBuffer);
    ID3D12RootSignature* GetRootSignature() const { return mRootSignature.Get(); }
    ID3D12PipelineState* GetPSO() const { return mPSO.Get(); }

private:
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildPSO();
    void BuildWavePSO();

    void BuildDeferredRootSignature();
    void BuildDeferredShaders();
    void BuildDeferredPSO();

private:
    ID3D12Device* md3dDevice = nullptr;
    DXGI_FORMAT mFormats[3];
    DXGI_FORMAT mDepthStencilFormat;

    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12PipelineState> mPSO = nullptr;
    ComPtr<ID3D12PipelineState> mWavePSO = nullptr;

    ComPtr<ID3DBlob> mvsByteCode = nullptr;
    ComPtr<ID3DBlob> mpsByteCode = nullptr;
    ComPtr<ID3DBlob> mWaveVSByteCode = nullptr;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    GBuffer* mGBuffer = nullptr;
    ComPtr<ID3D12PipelineState> mDeferredPSO;
    ComPtr<ID3D12RootSignature> mDeferredRootSig;
    ComPtr<ID3DBlob> mDeferredVSByteCode;
    ComPtr<ID3DBlob> mDeferredPSByteCode;

    bool mMsaaState;
    UINT mMsaaQuality;
};
