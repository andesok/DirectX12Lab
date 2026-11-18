#pragma once

// =====================================================================
// ДЗ (Terrain): ландшафт из тайлов карты высот + квадродерево LOD + frustum culling
//
// Загрузка: папка Terrain/ с тайлами из генератора (Gaea / World Machine / ...):
//     height_x0_y0.png, height_x1_y0.png, ...   — 8/16-битные PNG
//     height_x0_y0.r16 / .raw                    — 16-битный RAW (little-endian, квадратный)
//     color_x0_y0.png ...                        — (необязательно) цветовая карта
// Порядок "_y0_x0" тоже понимается; одиночный файл height.png / height.r16 — тоже.
// Тайлы склеиваются в одну карту (общая кромка в 1 пиксель определяется сама).
// Если файлов нет — генерируется процедурный ландшафт, чтобы приложение запускалось.
//
// GPU: одна текстура высот R16_UNORM, карта нормалей RGBA8 (считается из высот),
//      цветовая карта RGBA8, общие вершинный/индексный буферы сетки 33x33.
// =====================================================================

#include "../framework/d3dUtil.h"
#include "../framework/UploadBuffer.h"
#include "TerrainQuadtree.h"
#include "Frustum.h"

#include <wincodec.h>
#include <filesystem>
#include <regex>
#include <map>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <stdexcept>
#include <cfloat>
#include <climits>
#include <functional>

#pragma comment(lib, "windowscodecs.lib")

struct TerrainSettings
{
    std::wstring Folder = L"Terrain/";              // где лежат тайлы
    std::wstring ShaderPath = L"Shaders/terrain.hlsl";
    float WorldSize = 24000.0f;   // размер всей карты по большей стороне (единицы сцены)
    float HeightScale = 3000.0f;  // перепад высот: белый пиксель - чёрный пиксель
    UINT  GridDim = 32;           // квадов в стороне общей сетки (чётное)
    float LodDistance = 4.0f;     // K в ranges[l] = K * leafSize * 2^l
    float FlattenRadius = 0.0f;   // > 0 — выровнять площадку под Sponza
};

// b0 в terrain.hlsl
struct TerrainPassConstants
{
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();

    DirectX::XMFLOAT3 EyePosW = { 0, 0, 0 };
    float HeightScale = 1.0f;

    DirectX::XMFLOAT2 Origin = { 0, 0 };
    float BaseY = 0.0f;
    float TexelWorld = 1.0f;

    DirectX::XMFLOAT2 Extent = { 1, 1 };
    DirectX::XMFLOAT2 InvHeightmapSize = { 1, 1 };

    float GridDim = 32.0f;
    float HalfGridDim = 16.0f;
    float TwoOverGridDim = 1.0f / 16.0f;
    int   DebugLod = 0;

    int   HasColorMap = 0;
    DirectX::XMFLOAT3 Pad = { 0, 0, 0 };
};

// b1 — 8 корневых констант на узел
struct TerrainNodeConstants
{
    float OffsetX, OffsetZ;
    float Size;
    float Lod;
    float MorphX, MorphY;
    float Pad0, Pad1;
};
static_assert(sizeof(TerrainNodeConstants) == 8 * sizeof(UINT), "8 root constants");

class Terrain
{
public:
    static constexpr UINT SrvCount = 3;   // высоты, нормали, цвет

    // anchor: (x, y, z) — точка, над которой будет центр карты; y — высота пола (Sponza)
    void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
        const TerrainSettings& settings, const DirectX::XMFLOAT3& anchor,
        const DXGI_FORMAT rtvFormats[3], DXGI_FORMAT dsvFormat, bool msaa, UINT msaaQuality)
    {
        mDevice = device;
        mSettings = settings;

        LoadHeightData();
        PlaceInWorld(anchor);
        BuildNormalMap();

        mQuadtree.Build(mHeights.data(), mWidth, mHeight, mTexelWorld, mSettings.HeightScale,
            mOrigin.x, mOrigin.y, mBaseY, mSettings.GridDim);
        mQuadtree.SetLodDistance(mSettings.LodDistance);

        BuildGridGeometry(cmdList);
        BuildTextures(cmdList);
        BuildPipeline(rtvFormats, dsvFormat, msaa, msaaQuality);

        mPassCB = std::make_unique<UploadBuffer<TerrainPassConstants>>(device, 1, true);

        wchar_t msg[512];
        swprintf_s(msg, L"Terrain: %s, %ux%u texels (%ux%u tiles%s), levels=%u, leaf=%.1f, texel=%.2f\n",
            mSourceInfo.c_str(), mWidth, mHeight, mTilesX, mTilesY,
            mSharedEdges ? L", shared edges" : L"", mQuadtree.LevelCount(),
            mQuadtree.NodeSize(0), mTexelWorld);
        OutputDebugStringW(msg);
    }

    void BuildSrvs(CD3DX12_CPU_DESCRIPTOR_HANDLE cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE gpu, UINT descriptorSize)
    {
        mSrvTable = gpu;
        ID3D12Resource* res[SrvCount] = { mHeightTex.Get(), mNormalTex.Get(), mColorTex.Get() };
        const DXGI_FORMAT formats[SrvCount] = { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM };
        for (UINT i = 0; i < SrvCount; ++i)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = formats[i];
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MostDetailedMip = 0;
            srv.Texture2D.MipLevels = 1;
            srv.Texture2D.ResourceMinLODClamp = 0.0f;
            mDevice->CreateShaderResourceView(res[i], &srv, cpu);
            cpu.Offset(1, descriptorSize);
        }
    }

    // Вызывать после FlushCommandQueue в Initialize
    void ReleaseUploaders()
    {
        mUploaders.clear();
        mGeo.VertexBufferUploader = nullptr;
        mGeo.IndexBufferUploader = nullptr;
    }

    // lodEye — откуда выбирать LOD (при заморозке — замороженная камера)
    // frustum == nullptr => без отсечения
    void Update(const DirectX::XMFLOAT3& lodEye, DirectX::FXMMATRIX viewProj,
        const Frustum* frustum, bool debugLod)
    {
        const auto t0 = std::chrono::high_resolution_clock::now();

        std::function<TerrainCull(const TerrainAabb&)> cull;
        if (frustum)
        {
            cull = [frustum](const TerrainAabb& b) -> TerrainCull
                {
                    DirectX::BoundingBox box;
                    box.Center = { (b.MinX + b.MaxX) * 0.5f, (b.MinY + b.MaxY) * 0.5f, (b.MinZ + b.MaxZ) * 0.5f };
                    box.Extents = { (b.MaxX - b.MinX) * 0.5f, (b.MaxY - b.MinY) * 0.5f + 1.0f, (b.MaxZ - b.MinZ) * 0.5f };
                    switch (frustum->TestAABB(box))
                    {
                    case FrustumTest::Outside: return TerrainCull::Outside;
                    case FrustumTest::Inside:  return TerrainCull::Inside;
                    default:                   return TerrainCull::Intersect;
                    }
                };
        }
        mQuadtree.Select(lodEye.x, lodEye.y, lodEye.z, cull, mDrawNodes, mStats);

        const auto t1 = std::chrono::high_resolution_clock::now();
        mSelectMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        TerrainPassConstants pc;
        DirectX::XMStoreFloat4x4(&pc.ViewProj, DirectX::XMMatrixTranspose(viewProj));
        pc.EyePosW = lodEye;
        pc.HeightScale = mSettings.HeightScale;
        pc.Origin = mOrigin;
        pc.BaseY = mBaseY;
        pc.TexelWorld = mTexelWorld;
        pc.Extent = { mQuadtree.ExtentX(), mQuadtree.ExtentZ() };
        pc.InvHeightmapSize = { 1.0f / (float)mWidth, 1.0f / (float)mHeight };
        pc.GridDim = (float)mSettings.GridDim;
        pc.HalfGridDim = (float)mSettings.GridDim * 0.5f;
        pc.TwoOverGridDim = 2.0f / (float)mSettings.GridDim;
        pc.DebugLod = debugLod ? 1 : 0;
        pc.HasColorMap = mHasColorMap ? 1 : 0;
        mPassCB->CopyData(0, pc);
    }

    // Рисует в уже привязанный G-Buffer. Куча mCbvHeap должна быть установлена.
    void Draw(ID3D12GraphicsCommandList* cmdList, bool wireframe) const
    {
        if (mDrawNodes.empty()) return;

        cmdList->SetGraphicsRootSignature(mRootSignature.Get());
        cmdList->SetPipelineState(wireframe ? mPsoWire.Get() : mPso.Get());
        cmdList->SetGraphicsRootConstantBufferView(0, mPassCB->Resource()->GetGPUVirtualAddress());
        cmdList->SetGraphicsRootDescriptorTable(2, mSrvTable);

        const D3D12_VERTEX_BUFFER_VIEW vbv = mGeo.VertexBufferView();
        const D3D12_INDEX_BUFFER_VIEW ibv = mGeo.IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const UINT quarterCount = mIndexCount / 4;
        for (const TerrainDrawNode& n : mDrawNodes)
        {
            const float* morph = mQuadtree.MorphConsts(n.Level);
            TerrainNodeConstants nc = { n.OffsetX, n.OffsetZ, n.Size, (float)n.Level, morph[0], morph[1], 0.0f, 0.0f };
            cmdList->SetGraphicsRoot32BitConstants(1, 8, &nc, 0);

            if (n.Quarter < 0)
                cmdList->DrawIndexedInstanced(mIndexCount, 1, 0, 0, 0);
            else
                cmdList->DrawIndexedInstanced(quarterCount, 1, quarterCount * (UINT)n.Quarter, 0, 0);
        }
    }

    void SetLodDistance(float k) { mQuadtree.SetLodDistance(k); }
    float LodDistance() const { return mQuadtree.LodDistance(); }
    const TerrainSelectStats& Stats() const { return mStats; }
    double SelectMs() const { return mSelectMs; }
    UINT LevelCount() const { return mQuadtree.LevelCount(); }
    const std::wstring& SourceInfo() const { return mSourceInfo; }

    // Высота поверхности в точке (мир) — пригодится, чтобы ходить по земле
    float HeightAt(float x, float z) const
    {
        const float fx = (std::max)(0.0f, (std::min)((x - mOrigin.x) / mTexelWorld, (float)(mWidth - 1)));
        const float fz = (std::max)(0.0f, (std::min)((z - mOrigin.y) / mTexelWorld, (float)(mHeight - 1)));
        const UINT x0 = (UINT)fx, z0 = (UINT)fz;
        const UINT x1 = (std::min)(x0 + 1, mWidth - 1), z1 = (std::min)(z0 + 1, mHeight - 1);
        const float tx = fx - x0, tz = fz - z0;
        const float h0 = H(x0, z0) * (1 - tx) + H(x1, z0) * tx;
        const float h1 = H(x0, z1) * (1 - tx) + H(x1, z1) * tx;
        return mBaseY + (h0 * (1 - tz) + h1 * tz) * mSettings.HeightScale;
    }

private:
    // ================================================================
    // Загрузка изображений
    // ================================================================
    template <class T>
    struct Image
    {
        UINT W = 0, H = 0;
        std::vector<T> Data;
    };

    static bool WicLoad(const std::wstring& path, const GUID& format, UINT bytesPerPixel,
        UINT& w, UINT& h, std::vector<uint8_t>& out)
    {
        using Microsoft::WRL::ComPtr;
        // S_FALSE / RPC_E_CHANGED_MODE — COM уже инициализирован, это нормально
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            return false;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder)))
            return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return false;
        if (FAILED(frame->GetSize(&w, &h))) return false;

        ComPtr<IWICFormatConverter> conv;
        if (FAILED(factory->CreateFormatConverter(&conv))) return false;
        if (FAILED(conv->Initialize(frame.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
            return false;

        const UINT stride = w * bytesPerPixel;
        out.resize((size_t)stride * h);
        return SUCCEEDED(conv->CopyPixels(nullptr, stride, (UINT)out.size(), out.data()));
    }

    static bool LoadHeightFile(const std::filesystem::path& path, Image<uint16_t>& img)
    {
        std::wstring ext = path.extension().wstring();
        for (auto& c : ext) c = (wchar_t)towlower(c);

        if (ext == L".r16" || ext == L".raw")
        {
            const auto bytes = std::filesystem::file_size(path);
            const UINT n = (UINT)std::lround(std::sqrt((double)bytes / 2.0));
            if ((uintmax_t)n * n * 2 != bytes) return false;          // только квадратные RAW
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
            img.W = img.H = n;
            img.Data.resize((size_t)n * n);
            const size_t read = fread(img.Data.data(), 2, img.Data.size(), f);
            fclose(f);
            return read == img.Data.size();
        }

        std::vector<uint8_t> raw;
        if (!WicLoad(path.wstring(), GUID_WICPixelFormat16bppGray, 2, img.W, img.H, raw)) return false;
        img.Data.resize((size_t)img.W * img.H);
        memcpy(img.Data.data(), raw.data(), raw.size());
        return true;
    }

    static bool LoadColorFile(const std::filesystem::path& path, Image<uint32_t>& img)
    {
        std::vector<uint8_t> raw;
        if (!WicLoad(path.wstring(), GUID_WICPixelFormat32bppRGBA, 4, img.W, img.H, raw)) return false;
        img.Data.resize((size_t)img.W * img.H);
        memcpy(img.Data.data(), raw.data(), raw.size());
        return true;
    }

    // Склейка тайлов (x, y) в одну картинку. shared — соседние тайлы делят кромку в 1 пиксель.
    template <class T>
    static Image<T> Stitch(const std::map<std::pair<int, int>, Image<T>>& tiles, int nx, int ny, bool shared)
    {
        const Image<T>& first = tiles.begin()->second;
        const UINT tw = first.W, th = first.H;
        const UINT stepX = shared ? tw - 1 : tw;
        const UINT stepY = shared ? th - 1 : th;

        Image<T> out;
        out.W = stepX * nx + (shared ? 1 : 0);
        out.H = stepY * ny + (shared ? 1 : 0);
        out.Data.assign((size_t)out.W * out.H, T{});

        for (const auto& [key, tile] : tiles)
        {
            if (tile.W != tw || tile.H != th)
                throw std::runtime_error("Terrain: tiles have different sizes");
            const UINT ox = key.first * stepX, oy = key.second * stepY;
            for (UINT y = 0; y < th; ++y)
                memcpy(&out.Data[(size_t)(oy + y) * out.W + ox], &tile.Data[(size_t)y * tw], tw * sizeof(T));
        }
        return out;
    }

    // Найти тайлы с префиксом (height / color) в папке
    static std::map<std::pair<int, int>, std::filesystem::path> FindTiles(const std::wstring& folder, const std::wstring& prefix)
    {
        std::map<std::pair<int, int>, std::filesystem::path> result;
        if (!std::filesystem::exists(folder)) return result;

        const std::wregex xy(L"^" + prefix + L"\\w*?_x(\\d+)_y(\\d+)\\.(png|r16|raw)$", std::regex_constants::icase);
        const std::wregex yx(L"^" + prefix + L"\\w*?_y(\\d+)_x(\\d+)\\.(png|r16|raw)$", std::regex_constants::icase);
        const std::wregex single(L"^" + prefix + L"\\w*\\.(png|r16|raw)$", std::regex_constants::icase);

        std::filesystem::path singleFile;
        for (const auto& e : std::filesystem::directory_iterator(folder))
        {
            if (!e.is_regular_file()) continue;
            const std::wstring name = e.path().filename().wstring();
            std::wsmatch m;
            if (std::regex_match(name, m, xy))
                result[{ std::stoi(m[1].str()), std::stoi(m[2].str()) }] = e.path();
            else if (std::regex_match(name, m, yx))
                result[{ std::stoi(m[2].str()), std::stoi(m[1].str()) }] = e.path();
            else if (std::regex_match(name, single))
                singleFile = e.path();
        }

        if (result.empty() && !singleFile.empty())
            result[{ 0, 0 }] = singleFile;

        // Нумерация может начинаться с 1 — сдвигаем к 0
        if (!result.empty())
        {
            int minX = INT_MAX, minY = INT_MAX;
            for (auto& [k, v] : result) { minX = (std::min)(minX, k.first); minY = (std::min)(minY, k.second); }
            if (minX != 0 || minY != 0)
            {
                std::map<std::pair<int, int>, std::filesystem::path> shifted;
                for (auto& [k, v] : result) shifted[{ k.first - minX, k.second - minY }] = v;
                result.swap(shifted);
            }
        }
        return result;
    }

    void LoadHeightData()
    {
        const auto files = FindTiles(mSettings.Folder, L"height");
        if (files.empty())
        {
            GenerateProceduralHeights(2049);
            mSourceInfo = L"PROCEDURAL (no height tiles in " + mSettings.Folder + L")";
            return;
        }

        std::map<std::pair<int, int>, Image<uint16_t>> tiles;
        int nx = 0, ny = 0;
        for (const auto& [key, path] : files)
        {
            Image<uint16_t> img;
            if (!LoadHeightFile(path, img))
                throw std::runtime_error("Terrain: cannot read height tile " + path.string());
            tiles[key] = std::move(img);
            nx = (std::max)(nx, key.first + 1);
            ny = (std::max)(ny, key.second + 1);
        }
        if ((int)tiles.size() != nx * ny)
            throw std::runtime_error("Terrain: tile grid has holes (expected " + std::to_string(nx * ny) +
                " tiles, found " + std::to_string(tiles.size()) + ")");

        // Делят ли соседние тайлы кромку? Сравниваем край (0,0) с краем соседа.
        mSharedEdges = false;
        const Image<uint16_t>& t00 = tiles[{ 0, 0 }];
        if (nx > 1)
        {
            const Image<uint16_t>& t10 = tiles[{ 1, 0 }];
            int maxDiff = 0;
            for (UINT y = 0; y < t00.H; ++y)
                maxDiff = (std::max)(maxDiff, std::abs((int)t00.Data[(size_t)y * t00.W + t00.W - 1] - (int)t10.Data[(size_t)y * t10.W]));
            mSharedEdges = maxDiff <= 2;
        }
        else if (ny > 1)
        {
            const Image<uint16_t>& t01 = tiles[{ 0, 1 }];
            int maxDiff = 0;
            for (UINT x = 0; x < t00.W; ++x)
                maxDiff = (std::max)(maxDiff, std::abs((int)t00.Data[(size_t)(t00.H - 1) * t00.W + x] - (int)t01.Data[x]));
            mSharedEdges = maxDiff <= 2;
        }

        const Image<uint16_t> full = Stitch(tiles, nx, ny, mSharedEdges);
        if (full.W > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || full.H > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            throw std::runtime_error("Terrain: stitched heightmap is larger than 16384 (DX12 limit)");

        mWidth = full.W;
        mHeight = full.H;
        mTilesX = nx;
        mTilesY = ny;
        mHeights.resize(full.Data.size());
        for (size_t i = 0; i < full.Data.size(); ++i)
            mHeights[i] = full.Data[i] / 65535.0f;

        mSourceInfo = std::to_wstring(tiles.size()) + L" tile(s) from " + mSettings.Folder;

        // ---- Цветовая карта (необязательно) ----
        const auto colorFiles = FindTiles(mSettings.Folder, L"colou?r");
        if (!colorFiles.empty())
        {
            std::map<std::pair<int, int>, Image<uint32_t>> ctiles;
            int cx = 0, cy = 0;
            for (const auto& [key, path] : colorFiles)
            {
                Image<uint32_t> img;
                if (!LoadColorFile(path, img))
                    throw std::runtime_error("Terrain: cannot read color tile " + path.string());
                ctiles[key] = std::move(img);
                cx = (std::max)(cx, key.first + 1);
                cy = (std::max)(cy, key.second + 1);
            }
            if (cx == nx && cy == ny && (int)ctiles.size() == nx * ny)
            {
                const bool cshared = mSharedEdges && ctiles.begin()->second.W == t00.W;
                mColor = Stitch(ctiles, cx, cy, cshared);
                mHasColorMap = true;
                mSourceInfo += L" + color";
            }
            else
            {
                OutputDebugStringW(L"Terrain: color tiles grid does not match height tiles, ignored\n");
            }
        }
    }

    // Запасной вариант: fBm из value noise
    void GenerateProceduralHeights(UINT n)
    {
        mWidth = mHeight = n;
        mTilesX = mTilesY = 1;
        mHeights.resize((size_t)n * n);

        auto hash = [](int x, int y) -> float
            {
                uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
                h = (h ^ (h >> 13)) * 1274126177u;
                return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
            };
        auto noise = [&](float x, float y) -> float
            {
                const int xi = (int)std::floor(x), yi = (int)std::floor(y);
                float tx = x - xi, ty = y - yi;
                tx = tx * tx * (3 - 2 * tx);
                ty = ty * ty * (3 - 2 * ty);
                const float a = hash(xi, yi), b = hash(xi + 1, yi), c = hash(xi, yi + 1), d = hash(xi + 1, yi + 1);
                return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
            };

        float mn = FLT_MAX, mx = -FLT_MAX;
        for (UINT y = 0; y < n; ++y)
            for (UINT x = 0; x < n; ++x)
            {
                float fx = x / (float)n * 6.0f, fy = y / (float)n * 6.0f;
                float amp = 1.0f, sum = 0.0f;
                for (int o = 0; o < 8; ++o)
                {
                    float v = 1.0f - std::fabs(noise(fx, fy) * 2.0f - 1.0f);   // ridged
                    sum += v * v * amp;
                    fx *= 2.03f; fy *= 2.03f; amp *= 0.5f;
                }
                mHeights[(size_t)y * n + x] = sum;
                mn = (std::min)(mn, sum); mx = (std::max)(mx, sum);
            }
        for (float& h : mHeights) h = (h - mn) / (mx - mn);
    }

    float H(UINT x, UINT z) const { return mHeights[(size_t)z * mWidth + x]; }

    // Центр карты — над anchor; поверхность в центре — на уровне anchor.y
    void PlaceInWorld(const DirectX::XMFLOAT3& anchor)
    {
        const UINT maxQuads = (std::max)(mWidth, mHeight) - 1;
        mTexelWorld = mSettings.WorldSize / (float)maxQuads;

        const float extX = mTexelWorld * (mWidth - 1), extZ = mTexelWorld * (mHeight - 1);
        mOrigin = { anchor.x - extX * 0.5f, anchor.z - extZ * 0.5f };

        const UINT cx = mWidth / 2, cz = mHeight / 2;
        const float centerH = H(cx, cz);

        // Площадка под Sponza: высоты плавно приводятся к высоте центра
        if (mSettings.FlattenRadius > 0.0f)
        {
            const float rIn = mSettings.FlattenRadius / mTexelWorld;
            const float rOut = rIn * 1.8f;
            for (UINT z = 0; z < mHeight; ++z)
                for (UINT x = 0; x < mWidth; ++x)
                {
                    const float d = std::sqrt(float((int)x - (int)cx) * ((int)x - (int)cx) + float((int)z - (int)cz) * ((int)z - (int)cz));
                    if (d >= rOut) continue;
                    float t = (d - rIn) / (rOut - rIn);
                    t = (std::max)(0.0f, (std::min)(1.0f, t));
                    t = t * t * (3 - 2 * t);
                    float& h = mHeights[(size_t)z * mWidth + x];
                    h = centerH + (h - centerH) * t;
                }
        }

        // Чуть ниже пола, чтобы не было z-fighting с полом Sponza
        mBaseY = anchor.y - centerH * mSettings.HeightScale - 2.0f;
    }

    void BuildNormalMap()
    {
        mNormals.resize((size_t)mWidth * mHeight);
        const float k = mSettings.HeightScale / (2.0f * mTexelWorld);
        for (UINT z = 0; z < mHeight; ++z)
            for (UINT x = 0; x < mWidth; ++x)
            {
                const UINT xl = x > 0 ? x - 1 : x, xr = x + 1 < mWidth ? x + 1 : x;
                const UINT zd = z > 0 ? z - 1 : z, zu = z + 1 < mHeight ? z + 1 : z;
                // производные по X и Z в мировых единицах
                const float dx = (H(xr, z) - H(xl, z)) * k * (2.0f / (float)(xr - xl));
                const float dz = (H(x, zu) - H(x, zd)) * k * (2.0f / (float)(zu - zd));
                float nx = -dx, ny = 1.0f, nz = -dz;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                nx /= len; ny /= len; nz /= len;
                const uint32_t r = (uint32_t)std::lround((nx * 0.5f + 0.5f) * 255.0f);
                const uint32_t g = (uint32_t)std::lround((ny * 0.5f + 0.5f) * 255.0f);
                const uint32_t b = (uint32_t)std::lround((nz * 0.5f + 0.5f) * 255.0f);
                mNormals[(size_t)z * mWidth + x] = r | (g << 8) | (b << 16) | (255u << 24);
            }
    }

    // ================================================================
    // GPU-ресурсы
    // ================================================================

    // Общая сетка (GridDim+1)^2 вершин. Индексы сгруппированы по четвертям:
    // [четверть 0][четверть 1][четверть 2][четверть 3] — так узел можно нарисовать
    // целиком одним вызовом или одну четверть (когда ребёнок рисуется сам).
    void BuildGridGeometry(ID3D12GraphicsCommandList* cmdList)
    {
        const UINT dim = mSettings.GridDim;
        const UINT half = dim / 2;

        std::vector<DirectX::XMFLOAT2> vertices;
        vertices.reserve((dim + 1) * (dim + 1));
        for (UINT z = 0; z <= dim; ++z)
            for (UINT x = 0; x <= dim; ++x)
                vertices.push_back({ x / (float)dim, z / (float)dim });

        std::vector<uint16_t> indices;
        indices.reserve(dim * dim * 6);
        for (UINT q = 0; q < 4; ++q)
        {
            const UINT qx = (q & 1) * half, qz = (q >> 1) * half;
            for (UINT z = qz; z < qz + half; ++z)
                for (UINT x = qx; x < qx + half; ++x)
                {
                    const uint16_t v00 = (uint16_t)(z * (dim + 1) + x);
                    const uint16_t v10 = (uint16_t)(v00 + 1);
                    const uint16_t v01 = (uint16_t)(v00 + dim + 1);
                    const uint16_t v11 = (uint16_t)(v01 + 1);
                    // по часовой стрелке при взгляде сверху (front face в D3D)
                    indices.insert(indices.end(), { v00, v01, v11, v00, v11, v10 });
                }
        }
        mIndexCount = (UINT)indices.size();

        const UINT vbSize = (UINT)(vertices.size() * sizeof(DirectX::XMFLOAT2));
        const UINT ibSize = (UINT)(indices.size() * sizeof(uint16_t));
        mGeo.Name = "TerrainGrid";
        mGeo.VertexBufferGPU = d3dUtil::CreateDefaultBuffer(mDevice, cmdList, vertices.data(), vbSize, mGeo.VertexBufferUploader);
        mGeo.IndexBufferGPU = d3dUtil::CreateDefaultBuffer(mDevice, cmdList, indices.data(), ibSize, mGeo.IndexBufferUploader);
        mGeo.VertexByteStride = sizeof(DirectX::XMFLOAT2);
        mGeo.VertexBufferByteSize = vbSize;
        mGeo.IndexFormat = DXGI_FORMAT_R16_UINT;
        mGeo.IndexBufferByteSize = ibSize;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateTexture(ID3D12GraphicsCommandList* cmdList,
        DXGI_FORMAT format, UINT w, UINT h, UINT bytesPerPixel, const void* data)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D12Resource> tex;

        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(format, w, h, 1, 1);
        const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));

        const UINT64 uploadSize = GetRequiredIntermediateSize(tex.Get(), 0, 1);
        const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        const CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        ComPtr<ID3D12Resource> upload;
        ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

        D3D12_SUBRESOURCE_DATA sub = {};
        sub.pData = data;
        sub.RowPitch = (LONG_PTR)w * bytesPerPixel;
        sub.SlicePitch = sub.RowPitch * h;
        UpdateSubresources(cmdList, tex.Get(), upload.Get(), 0, 0, 1, &sub);

        // Высоты читает вершинный шейдер => NON_PIXEL, цвет/нормали — пиксельный
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);

        mUploaders.push_back(upload);   // живёт до FlushCommandQueue
        return tex;
    }

    void BuildTextures(ID3D12GraphicsCommandList* cmdList)
    {
        std::vector<uint16_t> h16(mHeights.size());
        for (size_t i = 0; i < mHeights.size(); ++i)
            h16[i] = (uint16_t)std::lround((std::max)(0.0f, (std::min)(1.0f, mHeights[i])) * 65535.0f);

        mHeightTex = CreateTexture(cmdList, DXGI_FORMAT_R16_UNORM, mWidth, mHeight, 2, h16.data());
        mNormalTex = CreateTexture(cmdList, DXGI_FORMAT_R8G8B8A8_UNORM, mWidth, mHeight, 4, mNormals.data());

        if (mHasColorMap)
        {
            mColorTex = CreateTexture(cmdList, DXGI_FORMAT_R8G8B8A8_UNORM, mColor.W, mColor.H, 4, mColor.Data.data());
        }
        else
        {
            const uint32_t white = 0xFFFFFFFFu;
            mColorTex = CreateTexture(cmdList, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, &white);
        }

        // CPU-копии больше не нужны (кроме высот — для HeightAt)
        mNormals.clear(); mNormals.shrink_to_fit();
        mColor.Data.clear(); mColor.Data.shrink_to_fit();
    }

    void BuildPipeline(const DXGI_FORMAT rtvFormats[3], DXGI_FORMAT dsvFormat, bool msaa, UINT msaaQuality)
    {
        using Microsoft::WRL::ComPtr;

        // ---- Корневая сигнатура: b0 (CBV), b1 (8 констант), t0..t2, статический сэмплер s0 ----
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, SrvCount, 0);

        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsConstants(8, 1);
        params[2].InitAsDescriptorTable(1, &srvRange);

        const CD3DX12_STATIC_SAMPLER_DESC linearClamp(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        const CD3DX12_ROOT_SIGNATURE_DESC rsDesc(3, params, 1, &linearClamp,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, errors;
        const HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(), errors.GetAddressOf());
        if (errors) OutputDebugStringA((const char*)errors->GetBufferPointer());
        ThrowIfFailed(hr);
        ThrowIfFailed(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));

        // ---- Шейдеры ----
        ComPtr<ID3DBlob> vs = d3dUtil::CompileShader(mSettings.ShaderPath, nullptr, "VS", "vs_5_0");
        ComPtr<ID3DBlob> ps = d3dUtil::CompileShader(mSettings.ShaderPath, nullptr, "PS", "ps_5_0");

        static const D3D12_INPUT_ELEMENT_DESC layout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.InputLayout = { layout, _countof(layout) };
        pso.pRootSignature = mRootSignature.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 3;
        for (UINT i = 0; i < 3; ++i) pso.RTVFormats[i] = rtvFormats[i];
        pso.SampleDesc.Count = msaa ? 4 : 1;
        pso.SampleDesc.Quality = msaa ? (msaaQuality - 1) : 0;
        pso.DSVFormat = dsvFormat;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&mPso)));

        pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&mPsoWire)));
    }

private:
    ID3D12Device* mDevice = nullptr;
    TerrainSettings mSettings;

    // CPU-данные
    std::vector<float> mHeights;      // [0,1], mWidth * mHeight
    std::vector<uint32_t> mNormals;   // RGBA8
    Image<uint32_t> mColor;
    bool mHasColorMap = false;
    UINT mWidth = 0, mHeight = 0;
    UINT mTilesX = 0, mTilesY = 0;
    bool mSharedEdges = false;
    std::wstring mSourceInfo;

    float mTexelWorld = 1.0f;
    DirectX::XMFLOAT2 mOrigin = { 0, 0 };
    float mBaseY = 0.0f;

    TerrainQuadtree mQuadtree;
    std::vector<TerrainDrawNode> mDrawNodes;
    TerrainSelectStats mStats;
    double mSelectMs = 0.0;

    // GPU
    MeshGeometry mGeo;
    UINT mIndexCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> mHeightTex, mNormalTex, mColorTex;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mUploaders;
    D3D12_GPU_DESCRIPTOR_HANDLE mSrvTable = {};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPso, mPsoWire;
    std::unique_ptr<UploadBuffer<TerrainPassConstants>> mPassCB;
};
