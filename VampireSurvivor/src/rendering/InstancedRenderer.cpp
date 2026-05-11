#define NOMINMAX
#include "InstancedRenderer.h"
#include <d3dx12/d3dx12.h>
#include <stdexcept>
#include <string>
#include <DirectXMath.h>

using namespace DirectX;

// Reads a compiled CSO file from disk.
static std::vector<uint8_t> LoadCSO(const wchar_t* path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"rb");
    if (!f) throw std::runtime_error("InstancedRenderer: cannot open shader CSO");
    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    rewind(f);
    std::vector<uint8_t> buf(sz);
    fread(buf.data(), 1, sz, f);
    fclose(f);
    return buf;
}

inline void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("D3D12 call failed");
}

namespace VS {

// Unit cube: 24 unique vertices (4 per face) so normals are flat per-face.
// The cube sits from Y=0 to Y=1, centered at (0, 0.5, 0).
struct Vertex { float x, y, z; };

static const Vertex kCubeVerts[24] = {
    // +Z face
    {-0.5f, 0.f,  0.5f}, { 0.5f, 0.f,  0.5f}, { 0.5f, 1.f,  0.5f}, {-0.5f, 1.f,  0.5f},
    // -Z face
    { 0.5f, 0.f, -0.5f}, {-0.5f, 0.f, -0.5f}, {-0.5f, 1.f, -0.5f}, { 0.5f, 1.f, -0.5f},
    // -X face
    {-0.5f, 0.f, -0.5f}, {-0.5f, 0.f,  0.5f}, {-0.5f, 1.f,  0.5f}, {-0.5f, 1.f, -0.5f},
    // +X face
    { 0.5f, 0.f,  0.5f}, { 0.5f, 0.f, -0.5f}, { 0.5f, 1.f, -0.5f}, { 0.5f, 1.f,  0.5f},
    // +Y face (top)
    {-0.5f, 1.f,  0.5f}, { 0.5f, 1.f,  0.5f}, { 0.5f, 1.f, -0.5f}, {-0.5f, 1.f, -0.5f},
    // -Y face (bottom)
    {-0.5f, 0.f, -0.5f}, { 0.5f, 0.f, -0.5f}, { 0.5f, 0.f,  0.5f}, {-0.5f, 0.f,  0.5f},
};

static const uint16_t kCubeIdx[36] = {
     0, 1, 2,   0, 2, 3,   // +Z
     4, 5, 6,   4, 6, 7,   // -Z
     8, 9,10,   8,10,11,   // -X
    12,13,14,  12,14,15,   // +X
    16,17,18,  16,18,19,   // +Y
    20,21,22,  20,22,23,   // -Y
};

// -------------------------------------------------------------------------

void InstancedRenderer::Init(
    ID3D12Device*              device,
    ID3D12GraphicsCommandList* uploadCmd,
    DXGI_FORMAT                rtvFmt,
    DXGI_FORMAT                dsvFmt)
{
    CreateRootSignature(device);
    CreatePSO(device, rtvFmt, dsvFmt);
    CreateMesh(device, uploadCmd);
    CreateInstanceBuffer(device);
}

void InstancedRenderer::UpdateInstances(const std::vector<InstanceData>& instances) {
    m_instanceCount = static_cast<uint32_t>(
        std::min(instances.size(), static_cast<size_t>(MaxInstances)));
    if (m_instanceCount > 0)
        memcpy(m_instanceMapped, instances.data(), m_instanceCount * sizeof(InstanceData));
}

void InstancedRenderer::Render(ID3D12GraphicsCommandList* cmd, const XMMATRIX& viewProj) {
    if (m_instanceCount == 0) return;

    if (gpuCullingEnabled && m_cullPso) {
        RenderGPUCull(cmd, viewProj);
        return;
    }

    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());

    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, XMMatrixTranspose(viewProj));
    cmd->SetGraphicsRoot32BitConstants(0, 16, &vp, 0);
    cmd->SetGraphicsRootShaderResourceView(1, m_instanceBuf->GetGPUVirtualAddress());

    cmd->IASetVertexBuffers(0, 1, &m_vbView);
    cmd->IASetIndexBuffer(&m_ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmd->DrawIndexedInstanced(m_indexCount, m_instanceCount, 0, 0, 0);
}

// -------------------------------------------------------------------------

void InstancedRenderer::CreateRootSignature(ID3D12Device* device) {
    // [0] 16 root constants (b0) — viewProj matrix
    // [1] root SRV (t0)        — StructuredBuffer<InstanceData>
    CD3DX12_ROOT_PARAMETER params[2];
    params[0].InitAsConstants(16, 0); // 16 floats, b0
    params[1].InitAsShaderResourceView(0); // t0

    CD3DX12_ROOT_SIGNATURE_DESC desc(
        2, params, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSig)));
}

void InstancedRenderer::CreatePSO(
    ID3D12Device* device, DXGI_FORMAT rtvFmt, DXGI_FORMAT dsvFmt)
{
    auto vs = LoadCSO(L"instance_VSMain.cso");
    auto ps = LoadCSO(L"instance_PSMain.cso");

    D3D12_INPUT_ELEMENT_DESC inputElem = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = m_rootSig.Get();
    psoDesc.VS                    = { vs.data(), vs.size() };
    psoDesc.PS                    = { ps.data(), ps.size() };
    psoDesc.InputLayout           = { &inputElem, 1 };
    psoDesc.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = rtvFmt;
    psoDesc.DSVFormat             = dsvFmt;
    psoDesc.SampleDesc            = { 1, 0 };

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));
}

void InstancedRenderer::CreateMesh(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmd)
{
    const UINT vbSize = sizeof(kCubeVerts);
    const UINT ibSize = sizeof(kCubeIdx);
    m_indexCount = 36;

    auto heapProps  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto heapUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto vbDesc     = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
    auto ibDesc     = CD3DX12_RESOURCE_DESC::Buffer(ibSize);

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_vb)));
    ThrowIfFailed(device->CreateCommittedResource(
        &heapUpload, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vbUpload)));

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_ib)));
    ThrowIfFailed(device->CreateCommittedResource(
        &heapUpload, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ibUpload)));

    // Upload via upload heaps
    auto Upload = [](ID3D12Resource* upBuf, const void* data, size_t sz) {
        void* ptr;
        CD3DX12_RANGE range(0, 0);
        upBuf->Map(0, &range, &ptr);
        memcpy(ptr, data, sz);
        upBuf->Unmap(0, nullptr);
    };
    Upload(m_vbUpload.Get(), kCubeVerts, vbSize);
    Upload(m_ibUpload.Get(), kCubeIdx,   ibSize);

    cmd->CopyBufferRegion(m_vb.Get(), 0, m_vbUpload.Get(), 0, vbSize);
    cmd->CopyBufferRegion(m_ib.Get(), 0, m_ibUpload.Get(), 0, ibSize);

    auto vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_vb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    auto ibBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_ib.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    cmd->ResourceBarrier(1, &vbBarrier);
    cmd->ResourceBarrier(1, &ibBarrier);

    m_vbView = { m_vb->GetGPUVirtualAddress(), vbSize, sizeof(Vertex) };
    m_ibView = { m_ib->GetGPUVirtualAddress(), ibSize, DXGI_FORMAT_R16_UINT };
}

void InstancedRenderer::CreateInstanceBuffer(ID3D12Device* device) {
    const UINT sz = MaxInstances * sizeof(InstanceData);
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto desc      = CD3DX12_RESOURCE_DESC::Buffer(sz);

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_instanceBuf)));

    CD3DX12_RANGE range(0, 0);
    m_instanceBuf->Map(0, &range, reinterpret_cast<void**>(&m_instanceMapped));
}

// ---- GPU Culling (Phase-2) --------------------------------------------------

// Extract 6 world-space frustum planes from a row-vector VP matrix.
// Convention: p is inside if dot(plane.xyz, p) + plane.w >= 0.
static void ExtractFrustumPlanes(const XMMATRIX& vp, XMFLOAT4 planes[6]) {
    XMFLOAT4X4 M;
    XMStoreFloat4x4(&M, vp);

    // In DirectXMath row-major storage, column j = (M[0][j], M[1][j], M[2][j], M[3][j]).
    // For clip-space result = worldPos * vp (row-vector convention):
    //   clip.x = dot(worldPos, col0), clip.w = dot(worldPos, col3), etc.
    auto Col = [&](int j) {
        return XMVectorSet(M.m[0][j], M.m[1][j], M.m[2][j], M.m[3][j]);
    };

    XMVECTOR raw[6] = {
        XMVectorAdd(Col(3), Col(0)),       // Left   (clip.x + clip.w >= 0)
        XMVectorSubtract(Col(3), Col(0)),  // Right  (clip.w - clip.x >= 0)
        XMVectorAdd(Col(3), Col(1)),       // Bottom (clip.y + clip.w >= 0)
        XMVectorSubtract(Col(3), Col(1)), // Top    (clip.w - clip.y >= 0)
        Col(2),                            // Near   (clip.z >= 0)
        XMVectorSubtract(Col(3), Col(2)), // Far    (clip.w - clip.z >= 0)
    };

    for (int i = 0; i < 6; ++i) {
        float len = XMVectorGetX(XMVector3Length(raw[i]));
        if (len > 1e-6f) raw[i] = XMVectorScale(raw[i], 1.f / len);
        XMStoreFloat4(&planes[i], raw[i]);
    }
}

void InstancedRenderer::InitGPUCulling(ID3D12Device* device) {
    CreateCullPipeline(device);
}

void InstancedRenderer::CreateCullPipeline(ID3D12Device* device) {
    // Compute root signature:
    //   [0] 28 root constants (b0) — CullCB (6 planes + count + pad)
    //   [1] root SRV (t0)  — gInput  (StructuredBuffer<InstanceData>)
    //   [2] root UAV (u0)  — gOutput (RWStructuredBuffer<InstanceData>)
    //   [3] root UAV (u1)  — gDrawArgs
    CD3DX12_ROOT_PARAMETER params[4];
    params[0].InitAsConstants(28, 0);
    params[1].InitAsShaderResourceView(0);
    params[2].InitAsUnorderedAccessView(0);
    params[3].InitAsUnorderedAccessView(1);

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc(4, params);
    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(device->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_cullRootSig)));

    // Compute PSO
    auto cs = LoadCSO(L"gpu_cull_cs.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_cullRootSig.Get();
    psoDesc.CS             = { cs.data(), cs.size() };
    ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_cullPso)));

    auto heapDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto heapUpload  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // Culled instance buffer (UAV, DEFAULT heap)
    {
        auto d = CD3DX12_RESOURCE_DESC::Buffer(
            MaxInstances * sizeof(InstanceData),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ThrowIfFailed(device->CreateCommittedResource(
            &heapDefault, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&m_culledBuf)));
    }

    // Draw args buffer (UAV + INDIRECT_ARGUMENT, DEFAULT heap)
    {
        auto d = CD3DX12_RESOURCE_DESC::Buffer(
            sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ThrowIfFailed(device->CreateCommittedResource(
            &heapDefault, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&m_drawArgsBuf)));
    }

    // Reset buffer (UPLOAD heap): template with instanceCount = 0
    {
        auto d = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
        ThrowIfFailed(device->CreateCommittedResource(
            &heapUpload, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_drawArgsReset)));

        D3D12_DRAW_INDEXED_ARGUMENTS resetArgs{ m_indexCount, 0, 0, 0, 0 };
        void* ptr;
        CD3DX12_RANGE range(0, 0);
        m_drawArgsReset->Map(0, &range, &ptr);
        memcpy(ptr, &resetArgs, sizeof(resetArgs));
        m_drawArgsReset->Unmap(0, nullptr);
    }

    // Command signature for ExecuteIndirect (DrawIndexedInstanced, no root change)
    D3D12_INDIRECT_ARGUMENT_DESC argDesc{};
    argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
    sigDesc.ByteStride      = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    sigDesc.NumArgumentDescs = 1;
    sigDesc.pArgumentDescs  = &argDesc;
    ThrowIfFailed(device->CreateCommandSignature(
        &sigDesc, nullptr, IID_PPV_ARGS(&m_cmdSig)));
}

void InstancedRenderer::RenderGPUCull(
    ID3D12GraphicsCommandList* cmd, const XMMATRIX& viewProj)
{
    // 1. Reset instanceCount in draw args buffer
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_drawArgsBuf.Get(), m_drawArgsBufState, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b);
        m_drawArgsBufState = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    cmd->CopyBufferRegion(m_drawArgsBuf.Get(), 0, m_drawArgsReset.Get(), 0,
        sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));

    // 2. Dispatch compute shader (frustum cull)
    {
        D3D12_RESOURCE_BARRIER bs[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                m_drawArgsBuf.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(
                m_culledBuf.Get(),
                m_culledBufState,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        cmd->ResourceBarrier(2, bs);
        m_drawArgsBufState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_culledBufState   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    cmd->SetComputeRootSignature(m_cullRootSig.Get());
    cmd->SetPipelineState(m_cullPso.Get());

    struct CullCB {
        XMFLOAT4 planes[6];
        uint32_t count;
        float    pad[3];
    } cb{};
    ExtractFrustumPlanes(viewProj, cb.planes);
    cb.count = m_instanceCount;

    cmd->SetComputeRoot32BitConstants(0, 28, &cb, 0);
    cmd->SetComputeRootShaderResourceView(1, m_instanceBuf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, m_culledBuf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, m_drawArgsBuf->GetGPUVirtualAddress());

    uint32_t groups = (m_instanceCount + 63) / 64;
    cmd->Dispatch(groups, 1, 1);

    // UAV barrier: ensure CS writes complete before draw reads
    {
        D3D12_RESOURCE_BARRIER bs[2] = {
            CD3DX12_RESOURCE_BARRIER::UAV(m_culledBuf.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_drawArgsBuf.Get()),
        };
        cmd->ResourceBarrier(2, bs);
    }

    // 3. Transition for ExecuteIndirect
    {
        D3D12_RESOURCE_BARRIER bs[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                m_culledBuf.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(
                m_drawArgsBuf.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        };
        cmd->ResourceBarrier(2, bs);
        m_culledBufState   = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        m_drawArgsBufState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }

    // 4. Draw culled instances via ExecuteIndirect
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());

    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, XMMatrixTranspose(viewProj));
    cmd->SetGraphicsRoot32BitConstants(0, 16, &vp, 0);
    cmd->SetGraphicsRootShaderResourceView(1, m_culledBuf->GetGPUVirtualAddress());

    cmd->IASetVertexBuffers(0, 1, &m_vbView);
    cmd->IASetIndexBuffer(&m_ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmd->ExecuteIndirect(m_cmdSig.Get(), 1, m_drawArgsBuf.Get(), 0, nullptr, 0);
}

} // namespace VS
