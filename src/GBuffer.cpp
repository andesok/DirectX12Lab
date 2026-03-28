#include "../headers/GBuffer.h"
#include "../headers/d3dUtil.h"

GBuffer::GBuffer(ID3D12Device* device, UINT width, UINT height)
    : md3dDevice(device), mWidth(width), mHeight(height),
    mRtvDescriptorSize(0), mSrvDescriptorSize(0)
{
}

GBuffer::~GBuffer()
{
    for (int i = 0; i < BufferCount; ++i)
    {
        mBuffers[i].Reset();
    }
}

void GBuffer::BuildResources()
{
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = mWidth;
    texDesc.Height = mHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    for (int i = 0; i < BufferCount; ++i)
    {
        texDesc.Format = mFormats[i];

        // Оптимальное значение очистки
        CD3DX12_CLEAR_VALUE optClear(mFormats[i], mClearColors[i]);

        ThrowIfFailed(md3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, // Начальное состояние
            &optClear,
            IID_PPV_ARGS(&mBuffers[i])
        ));
    }
}

void GBuffer::BuildDescriptors(
    CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
    CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
    CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv)
{
    mhCpuSrv = hCpuSrv;
    mhGpuSrv = hGpuSrv;
    mhCpuRtv = hCpuRtv;

    mSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mRtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (int i = 0; i < BufferCount; ++i)
    {
        // 1. Создаем RTV (для записи)
        md3dDevice->CreateRenderTargetView(mBuffers[i].Get(), nullptr, RtvHandle(i));

        // 2. Создаем SRV (для чтения шейдером)
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = mFormats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(mhCpuSrv, i, mSrvDescriptorSize);
        md3dDevice->CreateShaderResourceView(mBuffers[i].Get(), &srvDesc, srvHandle);
    }
}

void GBuffer::TransitionToRenderTarget(ID3D12GraphicsCommandList* cmdList)
{
    for (int i = 0; i < BufferCount; ++i)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            mBuffers[i].Get(),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ResourceBarrier(1, &barrier);
    }
}

void GBuffer::TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList)
{
    for (int i = 0; i < BufferCount; ++i)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            mBuffers[i].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        cmdList->ResourceBarrier(1, &barrier);
    }
}

void GBuffer::Clear(ID3D12GraphicsCommandList* cmdList)
{
    for (int i = 0; i < BufferCount; ++i)
    {
        cmdList->ClearRenderTargetView(RtvHandle(i), mClearColors[i], 0, nullptr);
    }
}

void GBuffer::SetAsRenderTargets(ID3D12GraphicsCommandList* cmdList)
{
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
    for (int i = 0; i < BufferCount; ++i)
    {
        rtvHandles.push_back(RtvHandle(i));
    }

    cmdList->OMSetRenderTargets(rtvHandles.size(), rtvHandles.data(), false, nullptr);
}

void GBuffer::SetAsSRVs(ID3D12GraphicsCommandList* cmdList, UINT rootParameterIndex)
{
    // Создаем массив GPU дескрипторов
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> srvHandles;
    for (int i = 0; i < BufferCount; ++i)
    {
        srvHandles.push_back(SrvHandle(i));
    }

    // Устанавливаем дескрипторную таблицу
    cmdList->SetGraphicsRootDescriptorTable(rootParameterIndex, srvHandles[0]);
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::RtvHandle(int index) const {
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(mhCpuRtv, index, mRtvDescriptorSize);
}


D3D12_GPU_DESCRIPTOR_HANDLE GBuffer::SrvHandle(int index) const
{
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(mhGpuSrv, index, mSrvDescriptorSize);
}