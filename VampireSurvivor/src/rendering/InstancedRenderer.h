#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace VS {

// Per-entity instance data uploaded to the GPU each frame.
struct InstanceData {
    DirectX::XMFLOAT4X4 world;   // full world transform (scale+translate)
    DirectX::XMFLOAT4   color;
};

// Renders up to MaxInstances colored unit-cubes via a single instanced draw call.
// Phase-1: CPU uploads visible instances each frame, DrawIndexedInstanced.
// Phase-2 (TODO): GPU culling via gpu_cull_cs.hlsl + ExecuteIndirect.
class InstancedRenderer {
public:
    static constexpr uint32_t MaxInstances = 8192;

    void Init(
        ID3D12Device*              device,
        ID3D12GraphicsCommandList* uploadCmd,   // command list open for upload
        DXGI_FORMAT                rtvFmt,
        DXGI_FORMAT                dsvFmt);

    // Call once per frame before Render(). Copies data to upload buffer.
    void UpdateInstances(const std::vector<InstanceData>& instances);

    // Records draw commands into cmd. Expects RT and DSV already bound.
    void Render(
        ID3D12GraphicsCommandList*  cmd,
        const DirectX::XMMATRIX&   viewProj);

    // Phase-2: initialize GPU culling pipeline (call after Init).
    void InitGPUCulling(ID3D12Device* device);

    bool gpuCullingEnabled = false;

private:
    void CreateRootSignature(ID3D12Device* device);
    void CreatePSO(ID3D12Device* device, DXGI_FORMAT rtvFmt, DXGI_FORMAT dsvFmt);
    void CreateMesh(ID3D12Device* device, ID3D12GraphicsCommandList* cmd);
    void CreateInstanceBuffer(ID3D12Device* device);
    void CreateCullPipeline(ID3D12Device* device);
    void RenderGPUCull(ID3D12GraphicsCommandList* cmd, const DirectX::XMMATRIX& viewProj);

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // Instance data — upload heap so CPU can write directly.
    ComPtr<ID3D12Resource> m_instanceBuf;
    InstanceData*          m_instanceMapped = nullptr;
    uint32_t               m_instanceCount  = 0;

    // Unit-cube geometry
    ComPtr<ID3D12Resource>  m_vb;
    ComPtr<ID3D12Resource>  m_vbUpload;
    ComPtr<ID3D12Resource>  m_ib;
    ComPtr<ID3D12Resource>  m_ibUpload;
    D3D12_VERTEX_BUFFER_VIEW m_vbView{};
    D3D12_INDEX_BUFFER_VIEW  m_ibView{};
    uint32_t                 m_indexCount = 0;

    // GPU culling (Phase-2)
    ComPtr<ID3D12RootSignature>    m_cullRootSig;
    ComPtr<ID3D12PipelineState>    m_cullPso;
    ComPtr<ID3D12CommandSignature> m_cmdSig;
    ComPtr<ID3D12Resource>         m_culledBuf;
    ComPtr<ID3D12Resource>         m_drawArgsBuf;
    ComPtr<ID3D12Resource>         m_drawArgsReset;
    D3D12_RESOURCE_STATES          m_culledBufState   = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES          m_drawArgsBufState = D3D12_RESOURCE_STATE_COMMON;
};

} // namespace VS
