#define NOMINMAX
#include "GameApp.h"
#include "../ecs/Components.h"
#include <d3dx12/d3dx12.h>
#include <DirectXMath.h>
#include <dxgi1_6.h>
#include <stdexcept>
#include <string>
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

using namespace DirectX;

namespace VS {

GameApp::GameApp(UINT width, UINT height, std::wstring name)
    : DXSample(width, height, name)
{
}

// ---- OnInit -----------------------------------------------------------------

void GameApp::OnInit() {
    LoadPipeline();
    LoadAssets();
}

void GameApp::LoadPipeline() {
    UINT dxgiFlags = 0;
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&factory)));

    if (m_useWarpDevice) {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&m_device)));
    } else {
        ComPtr<IDXGIAdapter1> adapter;
        GetHardwareAdapter(factory.Get(), &adapter);
        ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&m_device)));
    }

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_commandQueue)));

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.BufferCount = FrameCount;
    scDesc.Width       = m_width;
    scDesc.Height      = m_height;
    scDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc  = { 1, 0 };

    ComPtr<IDXGISwapChain1> sc;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(), Win32Application::GetHwnd(),
        &scDesc, nullptr, nullptr, &sc));
    ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));
    ThrowIfFailed(sc.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = FrameCount;
    rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // DSV heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)));

    // RTVs
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT i = 0; i < FrameCount; ++i) {
            ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
            m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, m_rtvDescSize);
        }
    }

    // Depth stencil
    {
        D3D12_RESOURCE_DESC depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_D32_FLOAT, m_width, m_height);
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        CD3DX12_CLEAR_VALUE clearVal(DXGI_FORMAT_D32_FLOAT, 1.f, 0);
        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearVal, IID_PPV_ARGS(&m_depthStencil)));

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvViewDesc{};
        dsvViewDesc.Format        = DXGI_FORMAT_D32_FLOAT;
        dsvViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(
            m_depthStencil.Get(), &dsvViewDesc,
            m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // Command allocators and list
    for (UINT i = 0; i < FrameCount; ++i)
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])));

    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[m_frameIndex].Get(), nullptr,
        IID_PPV_ARGS(&m_commandList)));

    // Fence
    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValues[m_frameIndex] = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void GameApp::LoadAssets() {
    // Thread pool and renderer init (renderer uploads mesh via open command list)
    m_threadPool = std::make_unique<ThreadPool>();
    m_renderer   = std::make_unique<InstancedRenderer>();
    m_renderer->Init(
        m_device.Get(), m_commandList.Get(),
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT);

    // Close and execute command list (mesh upload)
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* cmds[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, cmds);
    WaitForGpu();
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    // GPU culling pipeline (loads gpu_cull_cs.cso)
    m_renderer->InitGPUCulling(m_device.Get());

    // ECS setup
    SetupECS();

    // ImGui
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = 1;
        desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_imguiSrvHeap)));
    }
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(Win32Application::GetHwnd());
    ImGui_ImplDX12_Init(
        m_device.Get(), FrameCount,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        m_imguiSrvHeap.Get(),
        m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart());

    ThrowIfFailed(m_commandList->Close());

    // Timer init
    QueryPerformanceFrequency(&m_frequency);
    QueryPerformanceCounter(&m_lastTime);
}

void GameApp::SetupECS() {
    m_world       = std::make_unique<World>();
    m_systemGraph = std::make_unique<SystemGraph>();

    // Player entity
    auto  player = m_world->reg.create();
    m_world->reg.emplace<Position>(player);
    m_world->reg.emplace<Velocity>(player);
    m_world->reg.emplace<Health>(player, Health{ 200.f, 200.f });
    m_world->reg.emplace<Player>(player);
    m_world->reg.emplace<RenderCircle>(player, RenderCircle{ {0.3f, 0.5f, 1.f, 1.f}, 0.7f });

    m_ctx.player = player;
    m_ctx.pool   = m_threadPool.get();

    RegisterSystems(*m_systemGraph, m_ctx, m_keys);
}

// ---- OnUpdate ---------------------------------------------------------------

void GameApp::OnUpdate() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = static_cast<float>(now.QuadPart - m_lastTime.QuadPart)
             / static_cast<float>(m_frequency.QuadPart);
    dt = std::min(dt, 0.05f); // cap at 50ms
    m_lastTime  = now;
    m_totalTime += dt;

    m_systemGraph->run(*m_world, dt);

    // FPS (exponential moving average)
    float fps = (dt > 0.f) ? 1.f / dt : 0.f;
    m_fpsSmooth = m_fpsSmooth * 0.9f + fps * 0.1f;

    // ImGui frame
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame(1.0f, 1.0f);
    ImGui::NewFrame();

    ImGui::SetNextWindowPos({ 10, 10 }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ 220, 0 }, ImGuiCond_Always);
    ImGui::Begin("Stats", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::TextColored({ 1.f, 0.85f, 0.f, 1.f }, "VampireSurvivor");
    ImGui::Separator();

    ImGui::Text("FPS        %5.1f", m_fpsSmooth);
    ImGui::Text("Frame      %5.2f ms", dt * 1000.f);
    ImGui::Separator();

    ImGui::Text("Entities   %5d", m_ctx.entityCount);
    ImGui::Text("Enemies    %5d / %d", m_ctx.enemyCount, m_ctx.maxEnemies);
    ImGui::Text("Threads    %5d", static_cast<int>(m_threadPool->size()));
    ImGui::Separator();

    if (m_ctx.player != entt::null) {
        const auto& pl = m_world->reg.get<Player>(m_ctx.player);
        const auto& hp = m_world->reg.get<Health>(m_ctx.player);
        ImGui::Text("Level      %5d", pl.level);
        ImGui::Text("XP         %5.0f", pl.xp);
        ImGui::ProgressBar(hp.hp / hp.maxHp, { -1, 0 }, "HP");
    }
    ImGui::Separator();

    ImGui::SliderInt("Max enemies", &m_ctx.maxEnemies, 100, 5000);
    ImGui::Separator();
    ImGui::Checkbox("GPU Culling", &m_renderer->gpuCullingEnabled);

    ImGui::End();
    ImGui::Render();
}

// ---- OnRender ---------------------------------------------------------------

void GameApp::OnRender() {
    // Upload instance data computed by RenderSync system
    m_renderer->UpdateInstances(m_ctx.instances);

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

    PopulateCommandList();

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* cmds[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, cmds);

    ThrowIfFailed(m_swapChain->Present(1, 0));
    MoveToNextFrame();
}

void GameApp::PopulateCommandList() {
    // Camera: orbits slightly above and behind the player.
    XMFLOAT3 playerPos{};
    if (m_ctx.player != entt::null) {
        const auto& pp = m_world->reg.get<Position>(m_ctx.player);
        playerPos = { pp.x, 0.f, pp.z };
    }
    XMVECTOR eye    = XMVectorSet(playerPos.x, 30.f, playerPos.z - 25.f, 1.f);
    XMVECTOR target = XMVectorSet(playerPos.x, 0.f, playerPos.z, 1.f);
    XMVECTOR up     = XMVectorSet(0.f, 1.f, 0.f, 0.f);

    XMMATRIX view   = XMMatrixLookAtRH(eye, target, up);
    XMMATRIX proj   = XMMatrixPerspectiveFovRH(
        XM_PIDIV4, static_cast<float>(m_width) / m_height, 0.1f, 500.f);
    XMMATRIX vp     = view * proj;

    // Transition RT: present → render target
    auto toRT = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &toRT);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        static_cast<INT>(m_frameIndex), m_rtvDescSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    const float clear[] = { 0.05f, 0.05f, 0.08f, 1.f };
    m_commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    D3D12_VIEWPORT vp2{ 0, 0, (float)m_width, (float)m_height, 0.f, 1.f };
    D3D12_RECT     sci{ 0, 0, (LONG)m_width, (LONG)m_height };
    m_commandList->RSSetViewports(1, &vp2);
    m_commandList->RSSetScissorRects(1, &sci);

    m_renderer->Render(m_commandList.Get(), vp);

    // ImGui draw
    ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());

    // Transition RT: render target → present
    auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &toPresent);
}

// ---- fence / frame management -----------------------------------------------

void GameApp::WaitForGpu() {
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValues[m_frameIndex]));
    ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
    WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    ++m_fenceValues[m_frameIndex];
}

void GameApp::MoveToNextFrame() {
    const UINT64 currentValue = m_fenceValues[m_frameIndex];
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentValue));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }
    m_fenceValues[m_frameIndex] = currentValue + 1;
}

// ---- OnDestroy / input ------------------------------------------------------

void GameApp::OnDestroy() {
    WaitForGpu();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CloseHandle(m_fenceEvent);
}

void GameApp::OnKeyDown(UINT8 key) { m_keys[key] = true; }
void GameApp::OnKeyUp(UINT8 key)   { m_keys[key] = false; }

} // namespace VS
