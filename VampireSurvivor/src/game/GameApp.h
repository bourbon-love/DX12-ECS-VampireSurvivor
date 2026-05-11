#pragma once
#include "DXSample.h"
#include "../ecs/World.h"
#include "../ecs/SystemGraph.h"
#include "../jobs/ThreadPool.h"
#include "../rendering/InstancedRenderer.h"
#include "Systems.h"
#include <memory>

namespace VS {

class GameApp : public DXSample {
public:
    GameApp(UINT width, UINT height, std::wstring name);

    void OnInit()    override;
    void OnUpdate()  override;
    void OnRender()  override;
    void OnDestroy() override;
    void OnKeyDown(UINT8 key) override;
    void OnKeyUp(UINT8 key)   override;

private:
    void LoadPipeline();
    void LoadAssets();
    void PopulateCommandList();
    void WaitForGpu();
    void MoveToNextFrame();

    void SetupECS();

    // ---- DX12 core ----------------------------------------------------------
    static const UINT FrameCount = 2;

    ComPtr<ID3D12Device>              m_device;
    ComPtr<IDXGISwapChain3>           m_swapChain;
    ComPtr<ID3D12CommandQueue>        m_commandQueue;
    ComPtr<ID3D12CommandAllocator>    m_commandAllocators[FrameCount];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap>      m_dsvHeap;
    ComPtr<ID3D12Resource>            m_renderTargets[FrameCount];
    ComPtr<ID3D12Resource>            m_depthStencil;
    UINT                              m_rtvDescSize = 0;
    UINT                              m_frameIndex  = 0;

    HANDLE                            m_fenceEvent = nullptr;
    ComPtr<ID3D12Fence>               m_fence;
    UINT64                            m_fenceValues[FrameCount]{};

    // ---- game systems -------------------------------------------------------
    std::unique_ptr<World>             m_world;
    std::unique_ptr<SystemGraph>       m_systemGraph;
    std::unique_ptr<ThreadPool>        m_threadPool;
    std::unique_ptr<InstancedRenderer> m_renderer;

    GameCtx m_ctx;

    // ---- ImGui --------------------------------------------------------------
    ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;

    // ---- timing & input -----------------------------------------------------
    LARGE_INTEGER m_frequency{};
    LARGE_INTEGER m_lastTime{};
    float         m_totalTime  = 0.f;
    float         m_fpsSmooth  = 0.f;
    bool          m_keys[256]{};
};

} // namespace VS
