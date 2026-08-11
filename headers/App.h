#pragma once

#include "../framework/d3dApp.h"
#include "../framework/MathHelper.h"
#include "../framework/UploadBuffer.h"
#include "../framework/DDSTextureLoader.h" 
#include "../framework/RenderingSystem.h" 
#include "../framework/GBuffer.h"
#include "../headers/Camera.h"
#include "../headers/LightData.h"

#define NOMINMAX
#include <windows.h>
#include "../framework/tiny_obj_loader.h" 

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

static constexpr UINT kCBVSlot = 0;
static constexpr UINT kGBufferSrvBase = 2;
static constexpr UINT kTextureSrvBase = 5;

struct MeshTexture
{
    std::string Name;
    std::wstring Filename;
    ComPtr<ID3D12Resource> Resource = nullptr;
    ComPtr<ID3D12Resource> UploadHeap = nullptr;
};

struct TessellationConstants
{
    DirectX::XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 WorldInvTranspose = MathHelper::Identity4x4();
    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    float TessellationFactor = 16.0f;
    float DisplacementScale = 0.5f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
};

struct SubMeshTextures
{
    std::wstring albedoPath;
    std::wstring normalPath;
    std::wstring heightPath;
    int arrayIndex = -1;  // Индекс в Texture2DArray для альбедо
};
struct SubMeshInfo {
    std::string name;
    UINT indexCount;
    UINT startIndex;
    std::string materialName;
    SubMeshTextures textures;
    int textureIndex;
};

struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
    XMFLOAT3 Tangent;
    UINT TexIndex;
};

struct ObjectConstants
{
    XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
    XMFLOAT4X4 World = MathHelper::Identity4x4();
    float gTime = 0.0f;
    XMFLOAT3 padding;
};

class App : public D3DApp
{
public:
    App(HINSTANCE hInstance);
    App(const App& rhs) = delete;
    App& operator=(const App& rhs) = delete;
    ~App();

    virtual bool Initialize()override;

private:
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void BuildDescriptorHeaps();
    void BuildConstantBuffers();
    void LoadAllTextures();
    void BuildSampler();

    void BuildModelGeometry(std::string modelPath, std::string baseDir);
    void LoadTextureToArray(const std::wstring& path, UINT arrayIndex);
    void CreateTextureArraySRV();

private:
    std::unique_ptr<Camera> mCamera;
    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12DescriptorHeap> mCbvHeap;
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;
    ComPtr<ID3D12DescriptorHeap> mSamplerHeap;

    std::unique_ptr<UploadBuffer<PassConstants>> mPassCB;

    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB;

    std::unique_ptr<MeshGeometry> mGeo;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    ComPtr<ID3D12PipelineState> mPSO;

    XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.5f * XM_PI;
    float mPhi = XM_PIDIV4;
    float mRadius = 5.0f;
    int mUniqueTextureCount = 0;

    POINT mLastMousePos;
    std::vector<SubMeshInfo> mSubMeshInfos;

    std::unique_ptr<RenderingSystem> mRenderSystem;
    std::unique_ptr<GBuffer> mGBuffer;
    
    std::vector<Light> mLights;

    Light* mMainLight = nullptr;

    std::unique_ptr<UploadBuffer<Light>> mLightBuffer;
    UINT mLightBufferSize = 0;

    int mDisplacementTexIndex = -1;
    std::unique_ptr<UploadBuffer<TessellationConstants>> mTessCB;

    DirectX::XMFLOAT4 mAmbientLight = { 0.3f, 0.3f, 0.3f, 1.0f };

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mTextureUploadKeepAlive;
    // ============================================
    // TEXTURE2DARRAY РЕСУРСЫ
    // ============================================
    UINT mTextureCount = 0;                        // Количество текстур в массиве
    UINT mTextureWidth = 0;                       // Ширина текстур (все одинаковые)
    UINT mTextureHeight = 0;                      // Высота текстур (все одинаковые)
    DXGI_FORMAT mTextureFormat = DXGI_FORMAT_UNKNOWN;

    // Временные ресурсы для загрузки
    std::vector<SubMeshTextures> mSubMeshTextures;
    ComPtr<ID3D12Resource> mAlbedoArray;
    ComPtr<ID3D12Resource> mNormalArray;
    ComPtr<ID3D12Resource> mHeightArray;
};