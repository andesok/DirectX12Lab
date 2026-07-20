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
    void BeginFrame(ID3D12GraphicsCommandList* cmdList,
        D3D12_VIEWPORT& viewport,
        D3D12_RECT& scissor,
        GBuffer* gBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv);
    void DrawItem(ID3D12GraphicsCommandList* cmdList,
        const RenderItem& item,
        ID3D12DescriptorHeap* mainHeap,
        ID3D12DescriptorHeap* samplerHeap,
        bool isGeometryPass);
    void SetGBuffer(GBuffer* gBuffer) { mGBuffer = gBuffer; }
    void EndFrame(ID3D12GraphicsCommandList* cmdList,
        ID3D12DescriptorHeap* cbvHeap,
        ID3D12DescriptorHeap* samplerHeap,
        UINT descriptorSize,
        GBuffer* gBuffer,
        D3D12_GPU_VIRTUAL_ADDRESS passCBAddress,
        D3D12_GPU_VIRTUAL_ADDRESS lightBufferAddress,
        UINT numLights);
    ID3D12RootSignature* GetRootSignature() const { return mRootSignature.Get(); }

private:
    void BuildRootSignature();
    void BuildGeometryPSO();
    void BuildDeferredRootSignature();
    void BuildDeferredShaders();
    void BuildDeferredPSO();

private:
    ID3D12Device* md3dDevice;
    DXGI_FORMAT mFormats[3];
    DXGI_FORMAT mDepthStencilFormat;

    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12PipelineState> mGeometryPSO;

    ComPtr<ID3DBlob> mvsByteCode;
    ComPtr<ID3DBlob> mpsByteCode;
    ComPtr<ID3DBlob> mWaveVSByteCode ;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    GBuffer* mGBuffer;
    ComPtr<ID3D12PipelineState> mDeferredPSO;
    ComPtr<ID3D12RootSignature> mDeferredRootSig;
    ComPtr<ID3DBlob> mDeferredVSByteCode;
    ComPtr<ID3DBlob> mDeferredPSByteCode;

    bool mMsaaState;
    UINT mMsaaQuality;

    D3D12_VIEWPORT mViewport;
    D3D12_RECT mScissor;

};
