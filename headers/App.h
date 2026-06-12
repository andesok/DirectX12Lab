#pragma once
#include "../framework/d3dApp.h"
#include "../framework/MathHelper.h"
#include "../framework/UploadBuffer.h"
#include "../framework/DDSTextureLoader.h" 
#include "../framework/RenderingSystem.h" 
#include "../framework/GBuffer.h"
#include "../headers/Camera.h"
//#include "../headers/LightData.h"

#define NOMINMAX
#include <windows.h>
#include "../framework/tiny_obj_loader.h" 

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

static constexpr UINT kCBVSlot = 0;
static constexpr UINT kGBufferSrvBase = 2; // слоты 2,3,4
static constexpr UINT kTextureSrvBase = 5; // слоты 5+

struct MeshTexture
{
    std::string Name;
    std::wstring Filename;
    ComPtr<ID3D12Resource> Resource = nullptr;
    ComPtr<ID3D12Resource> UploadHeap = nullptr;
};

struct SubMeshInfo {
    std::string name;
    UINT indexCount;
    UINT startIndex;
    std::string materialName;
    std::wstring texturePath;
    UINT srvIndex;
    int textureIndex;        // ← Индекс текстуры (-1 если нет)
};

struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT2 TexCoord;
};

struct ObjectConstants
{
    XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
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
    void LoadTextureForMaterial(const SubMeshInfo& submesh, int srvIndex);
    void BuildTextureHeap();
    void BuildSampler();

    void BuildModelGeometry(std::string modelPath, std::string baseDir);

private:
    std::unique_ptr<Camera> mCamera;
    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12DescriptorHeap> mCbvHeap;
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;
    ComPtr<ID3D12DescriptorHeap> mSamplerHeap;

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

    //std::unique_ptr<UploadBuffer<LightConstants>> mLightCB;

    std::vector<std::unique_ptr<MeshTexture>> mTextures;
    std::unordered_map<std::string, UINT> mTextureIndexMap;
};