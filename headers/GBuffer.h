#pragma once
#include "../headers/d3dApp.h"
#include <wrl.h>   

class GBuffer {
public:
    // Количество текстур в G-буфере (например: Albedo, Normal, Position)
    static const int BufferCount = 3;

    GBuffer(ID3D12Device* device, UINT width, UINT height);
    ~GBuffer();

    // Создание ресурсов и дескрипторов
    void BuildResources();
    void BuildDescriptors(
        CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
        CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
        CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv);
    void Clear(ID3D12GraphicsCommandList* cmdList);

    // Установка G-Buffer как Render Targets
    void SetAsRenderTargets(ID3D12GraphicsCommandList* cmdList);

    // Установка G-Buffer текстур как Shader Resources
    void SetAsSRVs(ID3D12GraphicsCommandList* cmdList, UINT rootParameterIndex);

    // Подготовка к записи (переход в RENDER_TARGET)
    void TransitionToRenderTarget(ID3D12GraphicsCommandList* cmdList);
    // Подготовка к чтению шейдером (переход в SHADER_RESOURCE)
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList);

    // Геттеры для отрисовки
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(int index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvHandle(int index) const;

    DXGI_FORMAT GetFormat(int index) const { return mFormats[index]; }
    UINT GetWidth() const { return mWidth; }
    UINT GetHeight() const { return mHeight; }

private:
    ID3D12Device* md3dDevice = nullptr;
    UINT mWidth, mHeight;

    // Форматы текстур (обычно используют высокую точность для нормалей/позиций)
    DXGI_FORMAT mFormats[BufferCount] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,   // Albedo
        DXGI_FORMAT_R16G16B16A16_FLOAT, // Normals
        DXGI_FORMAT_R16G16B16A16_FLOAT  // Position/Depth
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> mBuffers[BufferCount];


    CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuRtv;   // Начало RTV в куче
    CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuSrv;   // Начало SRV в куче (CPU)
    CD3DX12_GPU_DESCRIPTOR_HANDLE mhGpuSrv;   // Начало SRV в куче (GPU)

    UINT mRtvDescriptorSize;
    UINT mSrvDescriptorSize;

    // Цвета очистки для каждого буфера
    float mClearColors[BufferCount][4] = {
        {0.0f, 0.0f, 0.0f, 1.0f},  // Albedo: чёрный
        {0.5f, 0.5f, 1.0f, 0.0f},  // Normals: (0,0,1) после распаковки
        {0.0f, 0.0f, 0.0f, 0.0f}   // World Position: ноль
    };
};