#pragma once
#include "../framework/d3dApp.h"
#include <wrl.h>   

class GBuffer {
public:
    static const int BufferCount = 3;

    GBuffer(ID3D12Device* device, UINT width, UINT height);
    ~GBuffer();

    void BuildResources();
    void BuildDescriptors(
        CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
        CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
        CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv);
    void Clear(ID3D12GraphicsCommandList* cmdList);

    void SetAsRenderTargets(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE* dsv = nullptr);

    void SetAsSRVs(ID3D12GraphicsCommandList* cmdList, UINT rootParameterIndex);

    void TransitionToRenderTarget(ID3D12GraphicsCommandList* cmdList);
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(int index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvHandle(int index) const;

    DXGI_FORMAT GetFormat(int index) const { return mFormats[index]; }
    UINT GetWidth() const { return mWidth; }
    UINT GetHeight() const { return mHeight; }

private:
    ID3D12Device* md3dDevice = nullptr;
    UINT mWidth, mHeight;

    DXGI_FORMAT mFormats[BufferCount] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> mBuffers[BufferCount];


    CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuRtv;
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuSrv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhGpuSrv;

    UINT mRtvDescriptorSize;
    UINT mSrvDescriptorSize;

    float mClearColors[BufferCount][4] = {
        {0.0f, 0.0f, 0.0f, 1.0f},  // Albedo: черный
        {0.5f, 0.5f, 1.0f, 0.0f},  // Normals: (0,0,1) после распаковки
        {0.0f, 0.0f, 0.0f, 0.0f}   // World Position: ноль
    };
};