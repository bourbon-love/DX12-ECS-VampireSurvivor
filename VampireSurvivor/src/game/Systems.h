#pragma once
#include "../ecs/World.h"
#include "../rendering/InstancedRenderer.h"
#include <entt/entt.hpp>
#include <vector>

namespace VS {
class ThreadPool;

// Shared context passed via closure to every system lambda.
// Lives in GameApp, referenced by systems through std::shared_ptr.
struct GameCtx {
    ThreadPool*               pool     = nullptr;
    entt::entity              player   = entt::null;

    // Spawn state
    float spawnTimer         = 0.f;
    float spawnInterval      = 0.8f;  // seconds between spawns (decreases over time)
    int   maxEnemies         = 5000;
    int   batchSpawnRequest  = 0;     // set by UI; consumed in one frame by sSpawn

    // Gathered each frame by RenderSyncSystem
    std::vector<InstanceData> instances;

    // Stats (read by GameApp for window title)
    int entityCount   = 0;
    int enemyCount    = 0;
};

// Registers all game systems into the given SystemGraph.
// The context must outlive the graph.
void RegisterSystems(class SystemGraph& graph, GameCtx& ctx, const bool* keys);

} // namespace VS
