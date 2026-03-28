#pragma once

#include "../headers/d3dApp.h"
#include "../headers/MathHelper.h"
#include "../headers/UploadBuffer.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct RenderConstants {
    XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
    float TotalTime = 0.0f;     // Для анимации
    XMFLOAT2 Tiling = { 1.0f, 1.0f }; // Для тайлинга
    float Padding;
};

struct RenderItem {
    MeshGeometry* Mesh = nullptr;
    std::string SubmeshName;
    UINT CBIndex = 0;   // Индекс в буфере констант
    UINT SRVIndex = 0;  // Индекс текстуры в куче
};

class RenderingSystem {
public:
    RenderingSystem(ID3D12Device* device, const DXGI_FORMAT gBufferFormats[3], DXGI_FORMAT depthBufferFormat, bool msaaState, UINT msaaQuality);

    ~RenderingSystem();

    // Инициализация графического конвейера
    void Initialize();

    // Подготовка кадра (установка вьюпортов, подносов с дескрипторами)
    void BeginFrame(ID3D12GraphicsCommandList* cmdList, D3D12_VIEWPORT& viewport, D3D12_RECT& scissor);

    // Отрисовка конкретной модели (с учетом текстуры и материала)
    void DrawItem(ID3D12GraphicsCommandList* cmdList,
        const RenderItem& item,
        ID3D12DescriptorHeap* mainHeap,
        ID3D12DescriptorHeap* samplerHeap);

private:
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildPSO();

private:
    ID3D12Device* md3dDevice = nullptr;
    DXGI_FORMAT mFormats[3];
    DXGI_FORMAT mDepthStencilFormat;

    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12PipelineState> mPSO = nullptr;

    ComPtr<ID3DBlob> mvsByteCode = nullptr;
    ComPtr<ID3DBlob> mpsByteCode = nullptr;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    bool mMsaaState;
    UINT mMsaaQuality;
};