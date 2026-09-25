#pragma once

#include "../framework/d3dUtil.h"
#include <cmath>
#include <cfloat>
#include <initializer_list>

// Каскадная карта теней: Texture2DArray (слой = каскад) + расчёт матриц каскадов.
class CascadedShadowMap
{
public:
    static constexpr UINT MaxCascades = 4;

    CascadedShadowMap(ID3D12Device* device, UINT size, UINT cascadeCount)
        : md3dDevice(device), mSize(size), mCascadeCount(cascadeCount)
    {
        mViewport = { 0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f };
        mScissor = { 0, 0, (LONG)size, (LONG)size };
        BuildResource();
        BuildDsvHeap();
    }

    // SRV кладётся в общую shader-visible кучу (слот задаёт App)
    void BuildSrv(CD3DX12_CPU_DESCRIPTOR_HANDLE cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE gpu)
    {
        mSrvGpu = gpu;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MostDetailedMip = 0;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.FirstArraySlice = 0;
        srv.Texture2DArray.ArraySize = mCascadeCount;
        srv.Texture2DArray.PlaneSlice = 0;
        srv.Texture2DArray.ResourceMinLODClamp = 0.0f;
        md3dDevice->CreateShaderResourceView(mShadowMap.Get(), &srv, cpu);
    }

    void TransitionToDepthWrite(ID3D12GraphicsCommandList* cmdList)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList->ResourceBarrier(1, &b);
    }

    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &b);
    }

    // Нелинейное (practical) разбиение + ортопроекция для каждого каскада.
    //   lambda = 0 -> равномерное разбиение, lambda = 1 -> логарифмическое.
    void UpdateCascades(DirectX::FXMMATRIX cameraView, float fovY, float aspect, float cameraNear,
        float shadowNear, float shadowFar, float lambda,
        const DirectX::XMFLOAT3& lightDir, const DirectX::BoundingBox& sceneBounds)
    {
        using namespace DirectX;

        const XMVECTOR L = XMVector3Normalize(XMLoadFloat3(&lightDir));
        const XMVECTOR up = std::fabs(XMVectorGetY(L)) > 0.99f
            ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
            : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        // Только поворот (начало координат в 0): при неподвижном свете
        // пространство света не меняется => привязка к текселям стабильна.
        const XMMATRIX lightRot = XMMatrixLookToLH(XMVectorZero(), L, up);

        // Глубина всей сцены в пространстве света: ближняя плоскость каскада
        // отодвигается до неё, чтобы не терять отбрасывающих тень вне кадра.
        XMFLOAT3 sceneCorners[8];
        sceneBounds.GetCorners(sceneCorners);
        float sceneMinZ = FLT_MAX, sceneMaxZ = -FLT_MAX;
        for (const XMFLOAT3& c : sceneCorners)
        {
            const float z = XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&c), lightRot));
            sceneMinZ = (std::fmin)(sceneMinZ, z);
            sceneMaxZ = (std::fmax)(sceneMaxZ, z);
        }

        const XMMATRIX invView = XMMatrixInverse(nullptr, cameraView);
        const float tanY = std::tan(fovY * 0.5f);
        const float tanX = tanY * aspect;

        float prevSplit = cameraNear;
        for (UINT i = 0; i < mCascadeCount; ++i)
        {
            // ---- 1. Граница каскада (practical split scheme) ----
            const float p = (float)(i + 1) / (float)mCascadeCount;
            const float logSplit = shadowNear * std::pow(shadowFar / shadowNear, p);
            const float uniSplit = shadowNear + (shadowFar - shadowNear) * p;
            const float split = lambda * logSplit + (1.0f - lambda) * uniSplit;

            // ---- 2. 8 углов под-пирамиды камеры в мировых координатах ----
            XMVECTOR corners[8];
            int k = 0;
            for (float d : { prevSplit, split })
                for (float sx : { -1.0f, 1.0f })
                    for (float sy : { -1.0f, 1.0f })
                        corners[k++] = XMVector3TransformCoord(
                            XMVectorSet(sx * d * tanX, sy * d * tanY, d, 1.0f), invView);

            // ---- 3. Ограничивающая сфера (не меняется при повороте камеры) ----
            XMVECTOR center = XMVectorZero();
            for (const XMVECTOR& c : corners) center += c;
            center /= 8.0f;

            float radius = 0.0f;
            for (const XMVECTOR& c : corners)
                radius = (std::fmax)(radius, XMVectorGetX(XMVector3Length(c - center)));
            radius = std::ceil(radius * 16.0f) / 16.0f;

            // ---- 4. Привязка центра к сетке текселей (против мерцания) ----
            const float texelWorld = 2.0f * radius / (float)mSize;
            XMFLOAT3 cLS;
            XMStoreFloat3(&cLS, XMVector3TransformCoord(center, lightRot));
            cLS.x = std::floor(cLS.x / texelWorld) * texelWorld;
            cLS.y = std::floor(cLS.y / texelWorld) * texelWorld;

            // ---- 5. Ортопроекция ----
            const float zn = (std::fmin)(sceneMinZ, cLS.z - radius) - 1.0f;
            float zf = (std::fmin)(cLS.z + radius, sceneMaxZ + 1.0f);
            if (zf <= zn + 1.0f) zf = zn + 1.0f;

            const XMMATRIX proj = XMMatrixOrthographicOffCenterLH(
                cLS.x - radius, cLS.x + radius,
                cLS.y - radius, cLS.y + radius,
                zn, zf);

            XMStoreFloat4x4(&mLightViewProj[i], lightRot * proj);
            mSplitFar[i] = split;
            mTexelWorld[i] = texelWorld;

            prevSplit = split;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT cascade) const
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(mDsvHeap->GetCPUDescriptorHandleForHeapStart(),
            cascade, mDsvDescriptorSize);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Srv() const { return mSrvGpu; }
    const D3D12_VIEWPORT& Viewport() const { return mViewport; }
    const D3D12_RECT& Scissor() const { return mScissor; }

    UINT Size() const { return mSize; }
    UINT CascadeCount() const { return mCascadeCount; }
    const DirectX::XMFLOAT4X4& LightViewProj(UINT i) const { return mLightViewProj[i]; }
    float SplitFar(UINT i) const { return mSplitFar[i]; }
    float TexelWorldSize(UINT i) const { return mTexelWorld[i]; }

private:
    void BuildResource()
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = mSize;
        desc.Height = mSize;
        desc.DepthOrArraySize = (UINT16)mCascadeCount;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;   // DSV: D32_FLOAT, SRV: R32_FLOAT
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(md3dDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&mShadowMap)));
    }

    void BuildDsvHeap()
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = mCascadeCount;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mDsvHeap)));
        mDsvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        for (UINT i = 0; i < mCascadeCount; ++i)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
            dsv.Format = DXGI_FORMAT_D32_FLOAT;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsv.Flags = D3D12_DSV_FLAG_NONE;
            dsv.Texture2DArray.MipSlice = 0;
            dsv.Texture2DArray.FirstArraySlice = i;   // один слой = один каскад
            dsv.Texture2DArray.ArraySize = 1;
            md3dDevice->CreateDepthStencilView(mShadowMap.Get(), &dsv, Dsv(i));
        }
    }

    ID3D12Device* md3dDevice = nullptr;
    UINT mSize = 2048;
    UINT mCascadeCount = MaxCascades;

    Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    UINT mDsvDescriptorSize = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE mSrvGpu = {};

    D3D12_VIEWPORT mViewport = {};
    D3D12_RECT mScissor = {};

    DirectX::XMFLOAT4X4 mLightViewProj[MaxCascades];
    float mSplitFar[MaxCascades] = {};
    float mTexelWorld[MaxCascades] = {};
};