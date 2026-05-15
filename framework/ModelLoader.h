#pragma once
#include "d3dApp.h"
#include <vector>
#include <string>

struct SubMeshInfo {
    std::string name;
    UINT indexCount;
    UINT startIndex;
    std::wstring texturePath;
};

class ModelLoader
{
public:
    ModelLoader(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);
    ~ModelLoader();

    // Загрузка .obj файла
    bool LoadFromFile(const std::string& modelPath, const std::string& baseDir);

    // Получить геометрию для рендеринга
    MeshGeometry* GetGeometry() const { return mGeo.get(); }

    // Получить информацию о подмешах
    const std::vector<SubMeshInfo>& GetSubMeshInfos() const { return mSubMeshInfos; }

    // Напечатать список подмешей (для отладки)
    void PrintSubMeshes() const;

private:
    ID3D12Device* mDevice;
    ID3D12GraphicsCommandList* mCmdList;

    std::unique_ptr<MeshGeometry> mGeo;
    std::vector<SubMeshInfo> mSubMeshInfos;
};