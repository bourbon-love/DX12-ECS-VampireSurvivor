# DX12 + EnTT ECS VampireSurvivor 项目详解

> 本教程逐文件、逐代码块地解释项目结构与运行机制。适合有一定 C++ 基础、想了解 DirectX 12 / ECS / 多线程如何在一个项目里协同工作的读者。

---

## 目录

1. [整体架构一览](#1-整体架构一览)
2. [程序入口 — `Main.cpp`](#2-程序入口--maincpp)
3. [Win32 框架层 — `src/framework/`](#3-win32-框架层--srcframework)
4. [ECS 层 — `src/ecs/`](#4-ecs-层--srcecs)
5. [游戏逻辑层 — `src/game/`](#5-游戏逻辑层--srcgame)
6. [多线程层 — `src/jobs/`](#6-多线程层--srcjobs)
7. [渲染层 — `src/rendering/`](#7-渲染层--srcrendering)
8. [着色器 — `shaders/`](#8-着色器--shaders)
9. [完整帧循环数据流](#9-完整帧循环数据流)
10. [关键技术要点汇总](#10-关键技术要点汇总)

---

## 1. 整体架构一览

```
Main.cpp                     ← 程序入口，创建 GameApp 并交给 Win32Application 运行
src/
  framework/                 ← Win32 窗口 + DX12 基础类（不含游戏逻辑）
    DXSample.h/cpp           ← 抽象基类，定义 OnInit/OnUpdate/OnRender/OnDestroy
    Win32Application.h/cpp   ← 消息循环、窗口创建、键鼠事件转发
    DXSampleHelper.h         ← ThrowIfFailed、ReadDataFromFile 等工具宏/函数
  ecs/                       ← ECS 核心（不依赖渲染）
    Components.h             ← 所有组件的 POD 数据定义
    World.h                  ← EnTT registry 的薄包装
    SystemGraph.h/cpp        ← 带依赖关系的系统调度器（拓扑排序）
  game/                      ← 游戏主逻辑
    GameApp.h/cpp            ← DX12 主应用，持有所有子系统，驱动帧循环
    Systems.h/cpp            ← 所有游戏 System 的实现（AI、碰撞、生成…）
  jobs/                      ← 线程池 + 任务图
    ThreadPool.h/cpp         ← 固定大小工作线程池
    TaskGraph.h/cpp          ← 按依赖层并行调度的任务图
  rendering/                 ← GPU 渲染
    InstancedRenderer.h/cpp  ← 单次 DrawInstanced / ExecuteIndirect 渲染所有实体
shaders/
  instance.hlsl              ← VS + PS：把 StructuredBuffer 里的实例画成彩色方块
  gpu_cull_cs.hlsl           ← CS：GPU 视锥剔除 + 生成 ExecuteIndirect 参数
third_party/
  entt/entt.hpp              ← EnTT 单头文件 ECS 库
```

**层与层之间的依赖方向**（单向，无环）：

```
Win32Application
      │
   GameApp  ←── SystemGraph ←── Systems
      │               │
   World           ThreadPool
      │
InstancedRenderer
      │
  Shaders (GPU)
```

---

## 2. 程序入口 — `Main.cpp`

```cpp
VS::GameApp app(1280, 720, L"VampireSurvivor — DX12 + EnTT ECS");
return Win32Application::Run(&app, hInstance, nCmdShow);
```

这两行是整个程序的起点：

- `GameApp` 继承自 `DXSample`，构造函数只记录宽高和标题，不做任何 DX12 初始化。
- `Win32Application::Run` 负责：
  1. 创建 Win32 窗口
  2. 调用 `app.OnInit()` 完成 DX12 和 ECS 初始化
  3. 进入 `while(msg != WM_QUIT)` 消息循环，每次循环调用 `app.OnUpdate()` 和 `app.OnRender()`
  4. 收到 WM_QUIT 后调用 `app.OnDestroy()` 清理

---

## 3. Win32 框架层 — `src/framework/`

### 3.1 `DXSample.h` — 抽象基类

```cpp
class DXSample {
public:
    virtual void OnInit()    = 0;
    virtual void OnUpdate()  = 0;
    virtual void OnRender()  = 0;
    virtual void OnDestroy() = 0;
    virtual void OnKeyDown(UINT8 key) {}
    virtual void OnKeyUp(UINT8 key)   {}
protected:
    UINT m_width, m_height;
    bool m_useWarpDevice = false;
    // ...
};
```

定义游戏循环的四个钩子。`GameApp` 重写它们来注入游戏/渲染逻辑。`m_useWarpDevice` 为 `true` 时使用软件渲染器（便于在没有独显的 CI 机器上测试）。

### 3.2 `Win32Application.cpp` — 消息循环

**`Run` 函数核心逻辑：**

```cpp
// 注册窗口类 → 创建窗口 → 调用 OnInit
sample->OnInit();
ShowWindow(hwnd, nCmdShow);

MSG msg{};
while (msg.message != WM_QUIT) {
    if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    } else {
        sample->OnUpdate();   // ← 游戏逻辑 + ImGui 构建 UI
        sample->OnRender();   // ← DX12 录制 + 提交 + Present
    }
}
sample->OnDestroy();
```

`PeekMessage` 是非阻塞的：有 Win32 消息时处理消息，没有时立刻进入 `OnUpdate/OnRender`，实现最大帧率循环。

**消息处理器关键部分：**

```cpp
case WM_KEYDOWN:
    sample->OnKeyDown(static_cast<UINT8>(wParam));
    break;
case WM_KEYUP:
    sample->OnKeyUp(static_cast<UINT8>(wParam));
    break;
```

键盘消息直接转发给 `GameApp::OnKeyDown/OnKeyUp`，在那里更新 `m_keys[256]` 数组，System 每帧读这个数组判断按键状态。

### 3.3 `DXSampleHelper.h` — 工具

```cpp
#define ThrowIfFailed(hr) \
    if (FAILED(hr)) throw HrException(hr, __FILE__, __LINE__)
```

所有 DX12 API 调用都用这个宏包裹。失败时立刻抛出异常，附带文件名和行号，方便调试。

---

## 4. ECS 层 — `src/ecs/`

ECS（实体-组件-系统）是这个项目的核心架构。核心思想：
- **实体（Entity）**：只是一个 ID（`entt::entity`），没有任何数据
- **组件（Component）**：纯数据，无逻辑
- **系统（System）**：纯逻辑，操作有特定组件的实体集合

### 4.1 `Components.h` — 所有组件

每个组件都是简单的 POD 结构体：

```cpp
struct Position  { float x = 0.f, z = 0.f; };          // 世界坐标（XZ 平面，俯视）
struct Velocity  { float vx = 0.f, vz = 0.f; };         // 速度（m/s）
struct Health    { float hp = 100.f, maxHp = 100.f; };  // 血量
struct RenderCircle {                                    // 渲染信息
    DirectX::XMFLOAT4 color{ 1,1,1,1 };
    float radius = 0.5f;
};
```

**标签/状态组件**（除了数据还携带角色标识）：

```cpp
struct Player {
    float speed        = 7.f;     // 移动速度
    float fireRate     = 0.25f;   // 射击间隔（秒），升级后缩短
    float fireCooldown = 0.f;     // 当前冷却剩余时间
    float xp           = 0.f;
    int   level        = 1;
};

struct Enemy {
    float speed       = 2.f;
    float damage      = 10.f;
    float xpDrop      = 5.f;     // 死亡时掉落的 XP
    float attackTimer = 0.f;     // 攻击冷却（1 秒间隔）
};

struct Projectile {
    float damage   = 25.f;
    float lifetime = 3.f;        // 超时自动销毁
};

struct XPOrb {
    float value    = 5.f;
    float lifetime = 20.f;       // 超时消失
};
```

**为什么用 POD 结构体？**  
EnTT 把同类型的组件在内存中连续存储（类似数组）。CPU 遍历时缓存命中率极高，这是 ECS 性能优于传统 OOP（对象内存分散）的根本原因。

### 4.2 `World.h` — Registry 包装

```cpp
struct World {
    entt::registry reg;
};
```

`entt::registry` 是 EnTT 的核心容器，管理所有实体和组件的存储。用 `World` 包一层的目的是：让 `GameApp` 不直接包含 `<entt/entt.hpp>`（避免大头文件污染所有编译单元）。

**常用 API 示例：**

```cpp
// 创建实体
auto e = world.reg.create();

// 附加组件
world.reg.emplace<Position>(e, 10.f, 5.f);

// 获取组件（引用）
auto& pos = world.reg.get<Position>(e);

// 遍历有某些组件的实体
world.reg.view<Enemy, Position>().each([](entt::entity e, Enemy& en, Position& pos) {
    // ...
});

// 销毁实体（同时回收所有组件）
world.reg.destroy(e);
```

### 4.3 `SystemGraph.h/cpp` — 带依赖的系统调度器

**设计目标：** 系统之间有执行顺序约束（例如 Movement 必须在 EnemyAI 之后运行），用拓扑排序自动算出合法执行顺序。

**数据结构：**

```cpp
struct Node {
    std::string              name;   // 系统名称
    SystemFn                 fn;     // std::function<void(World&, float)>
    std::vector<std::string> after;  // 必须在哪些系统之后运行
};
std::vector<Node> m_nodes;
std::vector<int>  m_order;   // 拓扑排序结果（节点下标的有序序列）
bool              m_dirty;   // add() 之后置 true，触发下次 run() 前重新排序
```

**`sort()` — Kahn 算法拓扑排序：**

```cpp
void SystemGraph::sort() {
    // 1. 建立名称→下标的映射
    std::unordered_map<std::string, int> idx;

    // 2. 统计每个节点的入度（有多少个前驱）
    std::vector<int> inDeg(n, 0);

    // 3. 建邻接表：dep → 依赖它的节点
    std::vector<std::vector<int>> adj(n);

    // 4. 把入度为 0 的节点入队（没有前驱，可以先跑）
    std::queue<int> q;

    // 5. BFS：出队一个，减少其后继的入度，入度降为 0 则入队
    while (!q.empty()) { ... }

    // 6. 如果排序结果数量 < 节点总数 → 有环 → 报错
    if (m_order.size() != n)
        throw std::runtime_error("SystemGraph: dependency cycle detected");
}
```

**`run()` — 每帧执行：**

```cpp
void SystemGraph::run(World& world, float dt) {
    if (m_dirty) sort();               // 首帧或有新增系统时才重新排序
    for (int i : m_order)
        m_nodes[i].fn(world, dt);      // 按拓扑顺序依次调用
}
```

**注册示例（来自 `Systems.cpp`）：**

```
PlayerInput                 ← 无前驱，最先执行
EnemyAI    → after: [PlayerInput]
Weapon     → after: [PlayerInput]
Movement   → after: [EnemyAI, Weapon]
Collision  → after: [Movement]
Projectile → after: [Collision]
XPOrb      → after: [Collision]
Spawn      → after: [Collision]
RenderSync → after: [Projectile, XPOrb, Spawn]  ← 最后执行，收集渲染数据
```

---

## 5. 游戏逻辑层 — `src/game/`

### 5.1 `Systems.h` — `GameCtx` 共享上下文

```cpp
struct GameCtx {
    ThreadPool*               pool;          // 线程池指针（AI 并行用）
    entt::entity              player;        // 玩家实体 ID
    float spawnTimer;                        // 距下次生成的倒计时
    float spawnInterval;                     // 生成间隔（随时间缩短）
    int   maxEnemies;                        // 最大敌人数（ImGui 滑条控制）
    std::vector<InstanceData> instances;     // RenderSync 填写，渲染器每帧读取
    int entityCount;                         // 当前帧实体总数（ImGui 显示）
    int enemyCount;                          // 当前帧敌人数（ImGui 显示）
};
```

`GameCtx` 通过 lambda 捕获传给每个 System，是各系统之间交换信息的"黑板"。

### 5.2 `Systems.cpp` — 所有游戏系统逐块解析

#### `sPlayerInput` — 玩家输入

```cpp
static void sPlayerInput(World& w, float dt, const bool* keys, GameCtx& ctx) {
    float dx = 0.f, dz = 0.f;
    if (keys['W'] || keys[VK_UP])    dz += 1.f;   // 前
    if (keys['S'] || keys[VK_DOWN])  dz -= 1.f;   // 后
    if (keys['A'] || keys[VK_LEFT])  dx += 1.f;   // 左
    if (keys['D'] || keys[VK_RIGHT]) dx -= 1.f;   // 右

    // 斜向移动归一化（防止斜向比直行快 √2 倍）
    float len = std::sqrtf(dx*dx + dz*dz);
    if (len > 0.f) { dx /= len; dz /= len; }

    vel.vx = dx * pl.speed;   // 写入 Velocity 组件
    vel.vz = dz * pl.speed;
}
```

读 `m_keys[256]` 数组（`Win32Application` 每帧维护），将方向映射到速度组件。

#### `sMovement` — 位置更新

```cpp
static void sMovement(World& w, float dt) {
    w.reg.view<Position, Velocity>().each([dt](Position& p, const Velocity& v) {
        p.x += v.vx * dt;   // 欧拉积分：位移 = 速度 × 时间
        p.z += v.vz * dt;
    });
}
```

遍历**所有**同时拥有 `Position` 和 `Velocity` 的实体（玩家、敌人、子弹都包含），统一更新位置。`dt` 确保移动速度与帧率无关。

#### `sEnemyAI` — 敌人 AI（多线程）

这是项目中最复杂的系统，分两条路径：

**步骤 1：收集敌人实体列表**

```cpp
auto view = w.reg.view<Enemy, Position, Velocity>();
std::vector<entt::entity> enemies;
view.each([&](entt::entity e, Enemy&, Position&, Velocity&) {
    enemies.push_back(e);
});
```

先把实体 ID 收集到 `vector`，便于按下标分批并行处理。

**步骤 2：分批派发到线程池**

```cpp
const int batchSize  = 128;
const int numBatches = (enemies.size() + 127) / 128;

if (ctx.pool && numBatches > 1) {
    std::vector<std::future<void>> futs;
    for (int b = 0; b < numBatches; ++b) {
        int start = b * batchSize;
        int end   = std::min(start + batchSize, (int)enemies.size());

        futs.push_back(ctx.pool->submit([&w, &enemies, &pp, start, end, dt] {
            for (int i = start; i < end; ++i) {
                // 计算从敌人到玩家的方向，更新 Velocity
                auto& pos = w.reg.get<Position>(enemies[i]);
                auto& vel = w.reg.get<Velocity>(enemies[i]);
                // ...归一化方向 × speed
            }
        }));
    }
    for (auto& f : futs) f.get();   // 等待所有批次完成
}
```

每批 128 个敌人独立在工作线程上计算。5000 个敌人 = 约 40 个并行任务。主线程和工作线程**同时**运行不同批次，利用多核 CPU。

**为什么这里用 `get<>` 是安全的？**  
每个线程操作的敌人下标范围不重叠，写的也是自己负责的实体的组件，不存在竞争条件。

**步骤 3：敌人数少时走单线程路径**

```cpp
} else {
    view.each([&pp, dt](Enemy& en, Position& pos, Velocity& vel) {
        // 同样的逻辑，但在主线程执行
    });
}
```

#### `sWeapon` — 自动射击

```cpp
// 找最近的敌人
entt::entity nearest = entt::null;
float minDist2 = 1e9f;
reg.view<Enemy, Position>().each([&](entt::entity e, Enemy&, const Position& ep) {
    float d2 = (ep.x - pp.x)*(ep.x - pp.x) + (ep.z - pp.z)*(ep.z - pp.z);
    if (d2 < minDist2) { minDist2 = d2; nearest = e; }
});

if (nearest == entt::null || minDist2 > 400.f) return; // 射程 20 单位

// 创建子弹实体
auto proj = reg.create();
reg.emplace<Position>(proj, pp);                              // 从玩家位置出发
reg.emplace<Velocity>(proj, Velocity{ dx*14.f, dz*14.f });   // 速度 14 m/s
reg.emplace<Projectile>(proj, Projectile{ 25.f, 3.f });       // 25 伤害，3 秒寿命
reg.emplace<RenderCircle>(proj, RenderCircle{ {1,1,0,1}, 0.2f }); // 黄色小圆
```

`fireCooldown` 倒计时到 0 才射击，升级时 `fireRate` 缩短，`fireCooldown` 也随之缩短。

#### `sProjectile` — 子弹生命周期

```cpp
reg.view<Projectile>().each([&](entt::entity e, Projectile& pr) {
    pr.lifetime -= dt;
    if (pr.lifetime <= 0.f) dead.push_back(e);
});
for (auto e : dead) reg.destroy(e);
```

每帧减少剩余寿命，寿命耗尽的子弹收集到 `dead` 列表，迭代结束后统一销毁（不在遍历过程中销毁，避免迭代器失效）。

#### `sCollision` — 碰撞检测

分两部分：

**子弹 vs 敌人：**

```cpp
reg.view<Projectile, Position>().each([&](entt::entity proj, Projectile& pr, const Position& projPos) {
    bool hit = false;
    reg.view<Enemy, Position, Health>().each([&](entt::entity enemy, Enemy&, const Position& ep, Health& eh) {
        if (hit) return;   // 一颗子弹只打一个敌人
        float d2 = 距离平方(projPos, ep);
        if (d2 < 0.64f) { // 合并半径 0.8 单位
            eh.hp -= pr.damage;
            if (eh.hp <= 0.f) deadEnemies.push_back(enemy);
            hit = true;
        }
    });
    if (hit) deadProj.push_back(proj);
});
```

使用"距离的平方"比较而不是"距离"，避免开平方的性能损耗。

**敌人 vs 玩家：**

```cpp
reg.view<Enemy, Position>().each([&](entt::entity, Enemy& en, const Position& ep) {
    float d2 = 距离平方(ep, pp);
    if (d2 < 1.0f) {   // 进入近战范围
        en.attackTimer -= dt;
        if (en.attackTimer <= 0.f) {
            ph.hp -= en.damage;
            en.attackTimer = 1.f; // 1 秒攻击一次
        }
    }
});
```

**敌人死亡掉落 XP 球：**

```cpp
for (auto e : deadEnemies) {
    auto pos = reg.get<Position>(e);
    auto orb = reg.create();
    reg.emplace<Position>(orb, pos);         // 在敌人死亡位置生成
    reg.emplace<XPOrb>(orb, { xpVal, 20.f });
    reg.emplace<RenderCircle>(orb, { {0,1,0.2f,1}, 0.25f }); // 绿色
    reg.destroy(e);
}
```

#### `sXPOrb` — XP 球拾取与升级

```cpp
reg.view<XPOrb, Position>().each([&](entt::entity e, XPOrb& xp, const Position& op) {
    xp.lifetime -= dt;
    float d2 = 距离平方(op, pp);
    if (d2 < pickupR2 || xp.lifetime <= 0.f)
        collected.push_back(e);    // 靠近或超时 → 收取
});

// 升级检查（每 100 XP 升一级）
int newLevel = 1 + (int)(pl.xp / 100.f);
if (newLevel > pl.level) {
    pl.level = newLevel;
    pl.fireRate = max(0.05f, 0.25f - (pl.level - 1) * 0.02f); // 射速随等级提升
}
```

#### `sSpawn` — 敌人生成

```cpp
if (cur >= ctx.maxEnemies) { ctx.spawnTimer = 0.1f; return; } // 已满，暂停

ctx.spawnInterval = max(0.1f, ctx.spawnInterval - 0.001f); // 随时间加速生成

// 在玩家周围半径 40 的圆环上随机选点生成
float angle = randf(0.f, 6.28f);
float ex = pp.x + 40.f * cosf(angle);
float ez = pp.z + 40.f * sinf(angle);

auto e = reg.create();
reg.emplace<Enemy>(e, Enemy{ randf(1.5f, 3.5f), 10.f, 5.f, 0.f }); // 随机速度
```

在屏幕外生成敌人，速度随机，压力随时间增大。

#### `sRenderSync` — 渲染数据收集

```cpp
auto Append = [&](const Position& pos, const RenderCircle& rc) {
    InstanceData d{};
    float s = rc.radius * 2.f;
    // 缩放矩阵 × 平移矩阵 → 世界矩阵
    XMMATRIX world = XMMatrixScaling(s, s, s) * XMMatrixTranslation(pos.x, 0.f, pos.z);
    // 转置后上传（HLSL 矩阵乘法约定不同）
    XMStoreFloat4x4(&d.world, XMMatrixTranspose(world));
    d.color = rc.color;
    ctx.instances.push_back(d);
};

// 玩家第一个（始终在列表最前面）
Append(w.reg.get<Position>(ctx.player), w.reg.get<RenderCircle>(ctx.player));

// 其余实体（敌人、子弹、XP 球）
w.reg.view<RenderCircle, Position>().each([&](entt::entity e, RenderCircle& rc, const Position& pos) {
    if (e == ctx.player) return;   // 玩家已经加过了
    Append(pos, rc);
});
```

这是 CPU 和 GPU 的桥梁：把 ECS 里的位置/颜色信息打包成 GPU 能读的 `InstanceData`，存在 `ctx.instances`，下一帧 `OnRender` 时上传。

### 5.3 `GameApp.cpp` — 主应用逐块解析

#### `LoadPipeline()` — DX12 基础设施

```
创建 DXGI Factory
    → 找硬件适配器（或 WARP 软件渲染）
    → D3D12CreateDevice（D3D feature level 11.0）
    → 创建命令队列（DIRECT 类型）
    → 创建交换链（2 缓冲，R8G8B8A8，Flip Discard）
    → 创建 RTV 描述符堆（2 个，对应 2 个后台缓冲）
    → 创建 DSV 描述符堆（1 个深度缓冲）
    → 创建深度缓冲资源（D32_FLOAT）
    → 为每个帧创建命令分配器（CommandAllocator）
    → 创建命令列表（CommandList）
    → 创建围栏（Fence）+ 事件句柄
```

**为什么需要 2 个 CommandAllocator？**  
双缓冲：GPU 执行第 N 帧时，CPU 同时录制第 N+1 帧。每帧有自己的分配器，不会互相干扰。

#### `LoadAssets()` — 渲染资源 + ECS 初始化

```cpp
m_threadPool = make_unique<ThreadPool>();      // 线程池（核心数-1 个工作线程）
m_renderer   = make_unique<InstancedRenderer>();
m_renderer->Init(..., m_commandList.Get(), ...); // 上传网格到 GPU（借用已打开的命令列表）

// 执行上传命令，等待 GPU 完成
m_commandList->Close();
m_commandQueue->ExecuteCommandLists(1, cmds);
WaitForGpu();  // 等围栏

m_renderer->InitGPUCulling(m_device.Get()); // 加载 gpu_cull_cs.cso

SetupECS(); // 创建玩家实体，注册所有 System
// ImGui 初始化...
```

#### `SetupECS()` — 创建玩家并注册系统

```cpp
auto player = m_world->reg.create();
m_world->reg.emplace<Position>(player);
m_world->reg.emplace<Velocity>(player);
m_world->reg.emplace<Health>(player, Health{ 200.f, 200.f }); // 玩家血量更多
m_world->reg.emplace<Player>(player);
m_world->reg.emplace<RenderCircle>(player, RenderCircle{ {0.3f,0.5f,1.f,1.f}, 0.7f }); // 蓝色大圆

m_ctx.player = player;
m_ctx.pool   = m_threadPool.get();

RegisterSystems(*m_systemGraph, m_ctx, m_keys); // 注册 9 个 System + 依赖关系
```

#### `OnUpdate()` — 每帧逻辑

```cpp
// 1. 计算 dt（用 Windows 高精度计时器）
float dt = (now - lastTime) / frequency;
dt = min(dt, 0.05f);  // 限制最大步长（调试暂停后不会爆炸）

// 2. 运行所有 System（按拓扑顺序）
m_systemGraph->run(*m_world, dt);

// 3. FPS 平滑（指数移动平均，α=0.1）
m_fpsSmooth = m_fpsSmooth * 0.9f + (1.f/dt) * 0.1f;

// 4. 构建 ImGui UI
ImGui_ImplDX12_NewFrame();
ImGui_ImplWin32_NewFrame(1.0f, 1.0f);
ImGui::NewFrame();
// ... 显示 FPS、实体数、血条、滑条、GPU 剔除开关 ...
ImGui::Render(); // 生成顶点数据（还没画，DrawData 在 OnRender 里提交）
```

#### `OnRender()` — 每帧渲染

```cpp
// 1. 把本帧实例数据上传到 GPU 上传堆
m_renderer->UpdateInstances(m_ctx.instances);

// 2. 重置命令分配器 + 命令列表
m_commandAllocators[m_frameIndex]->Reset();
m_commandList->Reset(allocator, nullptr);

// 3. 录制命令（见 PopulateCommandList）
PopulateCommandList();

// 4. 关闭并提交
m_commandList->Close();
m_commandQueue->ExecuteCommandLists(1, cmds);

// 5. Present + 推进帧索引
m_swapChain->Present(1, 0); // VSync
MoveToNextFrame();           // 信号围栏，等上一帧 GPU 完成
```

#### `PopulateCommandList()` — 命令录制

```cpp
// 1. 计算摄像机（跟随玩家的俯视角）
XMVECTOR eye    = {playerPos.x, 30.f, playerPos.z - 25.f};  // 高度 30，稍微偏后
XMVECTOR target = {playerPos.x, 0.f,  playerPos.z};
XMMATRIX vp     = view * proj;  // ViewProjection 矩阵

// 2. 资源屏障：后台缓冲 Present → RenderTarget
ResourceBarrier(PRESENT → RENDER_TARGET)

// 3. 清除颜色缓冲（深蓝黑）+ 深度缓冲
ClearRenderTargetView({0.05, 0.05, 0.08, 1.0})
ClearDepthStencilView(1.0)

// 4. 绑定 RT 和 DSV，设置视口和裁剪矩形
OMSetRenderTargets(rtv, dsv)
RSSetViewports / RSSetScissorRects

// 5. 渲染所有实例（一次 DrawInstanced 或 ExecuteIndirect）
m_renderer->Render(m_commandList.Get(), vp)

// 6. 渲染 ImGui
SetDescriptorHeaps(m_imguiSrvHeap)
ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList)

// 7. 资源屏障：RenderTarget → Present
ResourceBarrier(RENDER_TARGET → PRESENT)
```

#### `WaitForGpu()` 与 `MoveToNextFrame()` — 围栏同步

```cpp
// WaitForGpu：简单等待，确保 GPU 100% 完成（用于初始化阶段）
Signal(fence, fenceValues[frameIndex])
fence->SetEventOnCompletion(...)
WaitForSingleObjectEx(fenceEvent, INFINITE)

// MoveToNextFrame：高效的双缓冲等待
Signal(fence, currentValue)          // 告知 GPU：本帧结束
frameIndex = swapChain->GetCurrent() // 切换到下一帧缓冲
if (fence->GetCompletedValue() < fenceValues[frameIndex])
    Wait...  // 如果下一帧的 GPU 工作还没完成，等它
fenceValues[frameIndex] = currentValue + 1
```

`MoveToNextFrame` 只在 GPU 真的落后时才等待，大部分时间 CPU 和 GPU 并行运行。

---

## 6. 多线程层 — `src/jobs/`

### 6.1 `ThreadPool.h/cpp` — 工作线程池

**核心数据结构：**

```cpp
std::vector<std::thread>          m_workers;  // 固定数量的工作线程
std::queue<std::function<void()>> m_queue;    // 待执行任务队列
std::mutex                        m_mutex;    // 保护 queue 和 busy
std::condition_variable           m_workCv;   // 通知工作线程有新任务
std::condition_variable           m_idleCv;   // 通知 waitIdle() 所有任务完成
int                               m_busy;     // 正在执行中的任务数
bool                              m_stop;     // 析构时置 true，让线程退出
```

**构造函数：**

```cpp
ThreadPool::ThreadPool(size_t numThreads) {
    if (numThreads == 0)
        numThreads = max(1u, hardware_concurrency() - 1); // 留 1 个给主线程

    for (size_t i = 0; i < numThreads; ++i) {
        m_workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    unique_lock lock(m_mutex);
                    // 阻塞等待：有任务 或 需要退出
                    m_workCv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                    if (m_stop && m_queue.empty()) return; // 退出线程
                    task = move(m_queue.front());
                    m_queue.pop();
                }
                task(); // 执行任务（锁已释放）
                {
                    unique_lock lock(m_mutex);
                    if (--m_busy == 0 && m_queue.empty())
                        m_idleCv.notify_all(); // 通知 waitIdle()
                }
            }
        });
    }
}
```

每个工作线程永远循环：等条件变量 → 取任务 → 释放锁 → 执行。执行期间不持有锁，主线程可以继续提交新任务。

**`submit()` 模板函数：**

```cpp
template<typename F, typename... Args>
auto submit(F&& f, Args&&... args) -> std::future<...> {
    // packaged_task 把 f 包装成可以 get_future() 的对象
    auto task = make_shared<packaged_task<R()>>(bind(f, args...));
    auto fut  = task->get_future();
    {
        unique_lock lock(m_mutex);
        m_queue.push([task]() { (*task)(); });
        ++m_busy;
    }
    m_workCv.notify_one(); // 唤醒一个工作线程
    return fut;            // 调用者拿着 future 等结果
}
```

调用者用 `fut.get()` 等任务完成，这就是 `sEnemyAI` 末尾 `for (auto& f : futs) f.get();` 的机制。

**析构函数：**

```cpp
~ThreadPool() {
    { unique_lock lock(m_mutex); m_stop = true; }
    m_workCv.notify_all(); // 唤醒所有线程，让它们发现 m_stop=true 并退出
    for (auto& t : m_workers) t.join(); // 等待所有线程正常退出
}
```

### 6.2 `TaskGraph.h/cpp` — 层式并行任务图

与 `SystemGraph` 不同，`TaskGraph` 的任务**跨层并行**运行（同一层的任务同时提交给线程池）。

**分层算法：**

```cpp
// 每个任务的层 = max(所有依赖的层) + 1；无依赖的任务在第 0 层
std::function<int(size_t)> calcLayer = [&](size_t i) -> int {
    if (layer[i] >= 0) return layer[i]; // 已计算，直接返回（记忆化）
    int maxDep = -1;
    for (size_t d : m_tasks[i].deps)
        maxDep = max(maxDep, calcLayer(d));
    return layer[i] = maxDep + 1;
};
```

**执行：**

```cpp
for (int L = 0; L <= maxLayer; ++L) {
    vector<future<void>> futs;
    for (size_t i = 0; i < n; ++i)
        if (layer[i] == L)
            futs.push_back(pool.submit(m_tasks[i].fn)); // 同层任务并行提交
    for (auto& f : futs) f.get(); // 等本层全部完成再进入下一层
}
```

示例：任务 A、B 无依赖（层 0），任务 C 依赖 A 和 B（层 1）：

```
层 0: [A] [B]  ← 并行运行
        ↓   ↓
层 1:  [C]     ← A 和 B 都完成后运行
```

> **注意**：`TaskGraph` 目前在项目中已实现但未直接使用（`sEnemyAI` 直接用 `ThreadPool::submit`）。它作为更通用的并行框架保留，供后续扩展。

---

## 7. 渲染层 — `src/rendering/`

### 7.1 `InstancedRenderer.cpp` — 实例化渲染器

渲染器只有一种几何体：**单位立方体**（边长 1），通过 `world` 矩阵中的缩放和平移来表现不同大小和位置的实体。

#### 初始化：`Init()`

```
CreateRootSignature()  → 定义着色器参数绑定
CreatePSO()            → 编译渲染管线（VS+PS，背面剔除，深度测试）
CreateMesh()           → 创建立方体顶点/索引缓冲并上传到 GPU
CreateInstanceBuffer() → 在上传堆分配 MaxInstances(8192) × sizeof(InstanceData) 的缓冲
```

**根签名布局：**
- `[0]` 16 个根常量（b0）：`viewProj` 矩阵（4×4）
- `[1]` 根 SRV（t0）：`StructuredBuffer<InstanceData>`（实例数组）

**实例缓冲使用上传堆（Upload Heap）：** CPU 每帧直接 `memcpy` 写入，GPU 同帧读取。省去了显式的资源屏障，代价是带宽略低于默认堆（但对于每帧全部刷新的数据这是正确选择）。

#### CPU 渲染路径：`Render()`

```cpp
void InstancedRenderer::Render(ID3D12GraphicsCommandList* cmd, const XMMATRIX& viewProj) {
    if (gpuCullingEnabled && m_cullPso)
        return RenderGPUCull(cmd, viewProj); // GPU 剔除路径

    // CPU 路径：直接画所有实例
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());

    // 上传 viewProj（转置后作为 16 个根常量）
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, XMMatrixTranspose(viewProj));
    cmd->SetGraphicsRoot32BitConstants(0, 16, &vp, 0);

    // 绑定实例数据 SRV
    cmd->SetGraphicsRootShaderResourceView(1, m_instanceBuf->GetGPUVirtualAddress());

    // 绑定顶点/索引缓冲，设置图元拓扑
    cmd->IASetVertexBuffers(0, 1, &m_vbView);
    cmd->IASetIndexBuffer(&m_ibView);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 一次调用画所有实例
    cmd->DrawIndexedInstanced(m_indexCount, m_instanceCount, 0, 0, 0);
}
```

#### GPU 剔除路径：`RenderGPUCull()`

当 ImGui 中勾选"GPU Culling"时走这条路径，分 5 个阶段：

**阶段 1：重置间接参数缓冲的 instanceCount 为 0**

```cpp
// 把 m_drawArgsReset（预先写好 instanceCount=0 的缓冲）复制到 m_drawArgsBuf
CopyBufferRegion(drawArgsBuf, 0, drawArgsReset, 0, sizeof(DrawIndexedArgs))
```

**阶段 2：计算着色器 — 视锥剔除**

```cpp
// 提取视锥 6 个平面
ExtractFrustumPlanes(viewProj, planes);

// 设置计算管线
cmd->SetComputeRootSignature(m_cullRootSig.Get());
cmd->SetPipelineState(m_cullPso.Get());

// 上传参数（28 个 32 位常量：6 个平面 × 4 + count + 3 padding）
cmd->SetComputeRoot32BitConstants(0, 28, &cb, 0);

// 绑定输入实例（SRV t0）和输出缓冲（UAV u0 + u1）
cmd->SetComputeRootShaderResourceView(1, m_instanceBuf->GetGPUVirtualAddress());
cmd->SetComputeRootUnorderedAccessView(2, m_culledBuf->GetGPUVirtualAddress());
cmd->SetComputeRootUnorderedAccessView(3, m_drawArgsBuf->GetGPUVirtualAddress());

// 派发：每 64 个实例一个线程组
cmd->Dispatch((count + 63) / 64, 1, 1);
```

**阶段 3：UAV 屏障**（确保 CS 写入完成，再让后续着色器读取）

**阶段 4：资源状态转换**

```
culledBuf:   UAV → NON_PIXEL_SHADER_RESOURCE  （作为 VS 的 SRV）
drawArgsBuf: UAV → INDIRECT_ARGUMENT           （作为间接参数）
```

**阶段 5：ExecuteIndirect**

```cpp
cmd->SetGraphicsRootSignature(m_rootSig.Get());
cmd->SetPipelineState(m_pso.Get());
// viewProj、顶点/索引缓冲同 CPU 路径

// 绑定剔除后的实例缓冲（只含可见实例）
cmd->SetGraphicsRootShaderResourceView(1, m_culledBuf->GetGPUVirtualAddress());

// GPU 自己决定画多少个（instanceCount 由 CS 写入）
cmd->ExecuteIndirect(m_cmdSig.Get(), 1, m_drawArgsBuf.Get(), 0, nullptr, 0);
```

#### `ExtractFrustumPlanes()` — 视锥平面提取

```cpp
// DirectXMath 是行向量约定：v' = v × M
// 平面提取用 Gribb-Hartmann 方法（针对行向量约定）
// 矩阵 M 存储为行主序：M.m[row][col]
// 列向量 Col(j) = (M[0][j], M[1][j], M[2][j], M[3][j])

auto Col = [&](int j) {
    return XMVectorSet(M.m[0][j], M.m[1][j], M.m[2][j], M.m[3][j]);
};

XMVECTOR raw[6] = {
    Col(3) + Col(0),  // 左平面
    Col(3) - Col(0),  // 右平面
    Col(3) + Col(1),  // 下平面
    Col(3) - Col(1),  // 上平面
    Col(2),           // 近平面
    Col(3) - Col(2),  // 远平面
};
// 每个平面归一化（.xyz = 法线，.w = d，用于球体测试）
```

---

## 8. 着色器 — `shaders/`

### 8.1 `instance.hlsl` — 实例渲染着色器

**数据输入：**

```hlsl
struct InstanceData { float4x4 world; float4 color; };
StructuredBuffer<InstanceData> gInstances : register(t0); // 来自实例缓冲

cbuffer SceneCB : register(b0) { float4x4 gViewProj; }   // 来自根常量
```

**顶点着色器：**

```hlsl
PSInput VSMain(VSInput v) {
    InstanceData inst = gInstances[v.iid];  // v.iid = SV_InstanceID

    // inst.world 是 CPU 转置后的矩阵，行向量乘以矩阵
    float4 worldPos = mul(float4(v.position, 1.f), inst.world);
    o.svPos = mul(worldPos, gViewProj);
    o.color = inst.color;

    // 从 world 矩阵第 2 列取上方向（用于简单光照）
    float3 up = normalize(float3(inst.world[0][1], inst.world[1][1], inst.world[2][1]));
    o.normal = up;
}
```

**像素着色器：**

```hlsl
float4 PSMain(PSInput p) : SV_TARGET {
    float3 lightDir = normalize(float3(0.5f, 1.f, -0.3f));
    float  ndotl    = saturate(dot(p.normal, lightDir));
    // 漫反射：35% 环境光 + 65% 方向光
    float3 shaded   = p.color.rgb * (0.35f + 0.65f * ndotl);
    return float4(shaded, p.color.a);
}
```

### 8.2 `gpu_cull_cs.hlsl` — GPU 视锥剔除计算着色器

```hlsl
[numthreads(64, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gCount) return;  // 超出实例数，丢弃

    InstanceData inst = gInput[tid.x];

    if (InFrustum(inst.world)) {
        uint slot;
        // 原子加：多个线程同时写，每个线程拿到唯一的槽位
        InterlockedAdd(gDrawArgs[0].instanceCount, 1, slot);
        gOutput[slot] = inst;  // 写入可见实例
    }
}
```

**`InFrustum()` — 球体-视锥测试：**

```hlsl
bool InFrustum(float4x4 world) {
    // 从 world 矩阵提取中心点（CPU 转置后 _41/_42/_43 是平移分量）
    float3 center = float3(world._41, world._42, world._43);
    // 近似包围球半径（取对角线长度的一半）
    float  radius = 0.5f * length(float3(world._11, world._22, world._33));

    for (int i = 0; i < 6; ++i) {
        // 点到平面的符号距离 < -radius → 完全在平面外侧
        if (dot(gPlanes[i].xyz, center) + gPlanes[i].w < -radius)
            return false;
    }
    return true;
}
```

`InterlockedAdd` 保证多线程并发写入时不会出现竞争，每个可见实例拿到唯一的 `slot`，写入 `gOutput` 的不同位置，最终 `gOutput` 里是紧密排列的可见实例数组。

---

## 9. 完整帧循环数据流

```
─────────────────────── 每帧开始 ───────────────────────

[OnUpdate]
  QueryPerformanceCounter → 计算 dt

  SystemGraph::run()
    │
    ├─ PlayerInput    读 m_keys[] → 写 Player/Velocity 组件
    ├─ EnemyAI        读 Player.Position → 并行写 Enemy.Velocity（ThreadPool）
    ├─ Weapon         读 Enemy.Position 找最近目标 → create Projectile 实体
    ├─ Movement       读 Velocity → 写 Position（全部实体）
    ├─ Collision      读 Projectile/Enemy/Player.Position → 扣血、destroy 实体
    ├─ Projectile     读 Projectile.lifetime → destroy 超时子弹
    ├─ XPOrb          读 XPOrb/Player.Position → 拾取、升级
    ├─ Spawn          按间隔 create Enemy 实体
    └─ RenderSync     读所有 RenderCircle+Position → 写 ctx.instances[]

  ImGui::NewFrame() + 构建 UI 控件
  ImGui::Render()   ← 生成 DrawData（内存中）

[OnRender]
  InstancedRenderer::UpdateInstances(ctx.instances)
    └─ memcpy 到上传堆

  CommandList录制:
    ResourceBarrier(PRESENT → RENDER_TARGET)
    Clear RT + DSV
    OMSetRenderTargets

    ── GPU 剔除关闭 ──────────────────────────────
    SetRootSignature + PSO
    SetGraphicsRoot32BitConstants(viewProj)
    SetGraphicsRootSRV(instanceBuf)
    DrawIndexedInstanced(36, instanceCount, ...)
    ──────────────────────────────────────────────

    ── GPU 剔除开启 ──────────────────────────────
    CopyBufferRegion(reset instanceCount=0)
    ResourceBarrier → UAV
    SetComputeRootSignature + CullPSO
    Dispatch((count+63)/64, 1, 1)
      └─ 每线程: InFrustum() → InterlockedAdd → 写 culledBuf
    UAV Barrier
    ResourceBarrier(culledBuf → SRV, drawArgsBuf → INDIRECT)
    SetGraphicsRootSRV(culledBuf)
    ExecuteIndirect(cmdSig, drawArgsBuf)
    ──────────────────────────────────────────────

    SetDescriptorHeaps(imguiSrvHeap)
    ImGui_ImplDX12_RenderDrawData()

    ResourceBarrier(RENDER_TARGET → PRESENT)

  ExecuteCommandLists → 提交 GPU
  SwapChain::Present  → 显示
  MoveToNextFrame     → 围栏同步

─────────────────────── 下一帧 ────────────────────────
```

---

## 10. 关键技术要点汇总

### ECS 为什么快？

| 传统 OOP | ECS |
|---|---|
| `Enemy` 对象分散在堆上，遍历时随机跳内存 | 同类型组件连续存储，遍历时缓存命中率高 |
| 虚函数调用（vtable 间接跳转） | 直接调用 SystemFn，无虚函数 |
| 添加/删除特性需要继承或多重继承 | 运行时 `emplace/remove` 组件，无继承关系 |

### GPU 剔除的价值

- **无 GPU 剔除**：DrawIndexedInstanced 画 5000 个实例，VS 对每个实例都执行，光栅化阶段才丢弃不可见的
- **有 GPU 剔除**：CS 先过滤，只有可见实例进入 VS，顶点着色器调用次数大幅减少
- **ExecuteIndirect 的关键**：CPU 不需要读回剔除结果，instanceCount 直接由 GPU 写入参数缓冲，避免 GPU→CPU 数据传输的延迟

### 围栏同步双缓冲

```
帧 N:   CPU 录制 → Submit → Present → Signal(N)
帧 N+1: CPU 录制 → Submit → Present → Signal(N+1)
                                    ↓
                        如果 GPU 还在处理帧 N+1 的旧数据 → Wait
```

`MoveToNextFrame` 的精髓：只在 GPU **真的落后**时才等待，大多数帧 CPU 和 GPU 是真正并行运行的。

### 多线程安全性

`sEnemyAI` 中并行写组件之所以是安全的：
1. 每个批次操作的实体下标范围**不重叠**
2. 每个线程只写**自己负责的实体**的组件
3. 没有任何线程读另一个线程正在写的数据

如果需要跨线程共享数据（例如碰撞），必须串行化或用 `std::atomic` / `std::mutex`。
