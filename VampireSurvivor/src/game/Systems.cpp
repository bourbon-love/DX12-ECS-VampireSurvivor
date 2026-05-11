#define NOMINMAX
#include "Systems.h"
#include "../ecs/Components.h"
#include "../ecs/SystemGraph.h"
#include "../jobs/ThreadPool.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <future>
#include <random>

using namespace DirectX;

namespace VS {

// ---- helpers ----------------------------------------------------------------

static std::mt19937& rng() {
    static thread_local std::mt19937 g(std::random_device{}());
    return g;
}

static float randf(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng());
}

// ---- system implementations -------------------------------------------------

static void sPlayerInput(World& w, float dt, const bool* keys, GameCtx& ctx) {
    if (ctx.player == entt::null) return;
    auto& reg = w.reg;
    auto& vel = reg.get<Velocity>(ctx.player);
    auto& pl  = reg.get<Player>(ctx.player);

    float dx = 0.f, dz = 0.f;
    if (keys['W'] || keys[VK_UP])    dz += 1.f;
    if (keys['S'] || keys[VK_DOWN])  dz -= 1.f;
    if (keys['A'] || keys[VK_LEFT])  dx += 1.f;
    if (keys['D'] || keys[VK_RIGHT]) dx -= 1.f;

    float len = std::sqrtf(dx * dx + dz * dz);
    if (len > 0.f) { dx /= len; dz /= len; }
    vel.vx = dx * pl.speed;
    vel.vz = dz * pl.speed;
}

static void sMovement(World& w, float dt) {
    auto view = w.reg.view<Position, Velocity>();
    view.each([dt](Position& p, const Velocity& v) {
        p.x += v.vx * dt;
        p.z += v.vz * dt;
    });
}

// Enemy AI: move toward player. Parallelized with ThreadPool over batches.
static void sEnemyAI(World& w, float dt, GameCtx& ctx) {
    if (ctx.player == entt::null) return;
    const auto& pp = w.reg.get<Position>(ctx.player);

    // Collect enemy entities for parallel processing
    auto view = w.reg.view<Enemy, Position, Velocity>();
    std::vector<entt::entity> enemies;
    enemies.reserve(4096);
    for (auto e : view) enemies.push_back(e);
    ctx.enemyCount = static_cast<int>(enemies.size());

    if (enemies.empty()) return;

    const int batchSize = 128;
    const int numBatches = static_cast<int>((enemies.size() + batchSize - 1) / batchSize);

    if (ctx.pool && numBatches > 1) {
        std::vector<std::future<void>> futs;
        futs.reserve(numBatches);

        for (int b = 0; b < numBatches; ++b) {
            int start = b * batchSize;
            int end   = std::min(start + batchSize, static_cast<int>(enemies.size()));

            futs.push_back(ctx.pool->submit([&w, &view, &enemies, &pp, start, end, dt] {
                auto& reg = w.reg;
                for (int i = start; i < end; ++i) {
                    auto  e   = enemies[i];
                    auto& pos = reg.get<Position>(e);
                    auto& vel = reg.get<Velocity>(e);
                    auto& en  = reg.get<Enemy>(e);
                    float dx  = pp.x - pos.x;
                    float dz  = pp.z - pos.z;
                    float len = std::sqrtf(dx * dx + dz * dz);
                    if (len > 0.01f) { dx /= len; dz /= len; }
                    vel.vx = dx * en.speed;
                    vel.vz = dz * en.speed;
                }
            }));
        }
        for (auto& f : futs) f.get();
    } else {
        view.each([&pp, dt](Enemy& en, Position& pos, Velocity& vel) {
            float dx = pp.x - pos.x;
            float dz = pp.z - pos.z;
            float len = std::sqrtf(dx * dx + dz * dz);
            if (len > 0.01f) { dx /= len; dz /= len; }
            vel.vx = dx * en.speed;
            vel.vz = dz * en.speed;
        });
    }
}

// Auto-fire: player spawns projectiles toward nearest enemy.
static void sWeapon(World& w, float dt, GameCtx& ctx) {
    if (ctx.player == entt::null) return;
    auto& reg = w.reg;
    auto& pl  = reg.get<Player>(ctx.player);
    auto& pp  = reg.get<Position>(ctx.player);

    pl.fireCooldown -= dt;
    if (pl.fireCooldown > 0.f) return;

    // Find nearest enemy
    entt::entity nearest = entt::null;
    float minDist2 = 1e9f;
    for (auto e : reg.view<Enemy, Position>()) {
        auto& ep = reg.get<Position>(e);
        float d2 = (ep.x - pp.x) * (ep.x - pp.x) + (ep.z - pp.z) * (ep.z - pp.z);
        if (d2 < minDist2) { minDist2 = d2; nearest = e; }
    }

    if (nearest == entt::null || minDist2 > 400.f) return; // 20-unit range

    auto& np  = reg.get<Position>(nearest);
    float dx  = np.x - pp.x, dz = np.z - pp.z;
    float len = std::sqrtf(dx * dx + dz * dz);
    if (len < 0.001f) return;
    dx /= len; dz /= len;

    auto proj = reg.create();
    reg.emplace<Position>(proj, pp);
    reg.emplace<Velocity>(proj, Velocity{ dx * 14.f, dz * 14.f });
    reg.emplace<Projectile>(proj, Projectile{ 25.f, 3.f });
    reg.emplace<RenderCircle>(proj, RenderCircle{ {1.f, 1.f, 0.f, 1.f}, 0.2f });

    pl.fireCooldown = pl.fireRate;
}

static void sProjectile(World& w, float dt) {
    auto& reg = w.reg;
    std::vector<entt::entity> dead;

    for (auto e : reg.view<Projectile>()) {
        auto& pr = reg.get<Projectile>(e);
        pr.lifetime -= dt;
        if (pr.lifetime <= 0.f) dead.push_back(e);
    }
    for (auto e : dead) reg.destroy(e);
}

static void sCollision(World& w, float dt, GameCtx& ctx) {
    if (ctx.player == entt::null) return;
    auto& reg = w.reg;
    auto& pp  = reg.get<Position>(ctx.player);
    auto& ph  = reg.get<Health>(ctx.player);

    std::vector<entt::entity> deadEnemies;
    std::vector<entt::entity> deadProj;

    // Projectile–Enemy collision
    for (auto proj : reg.view<Projectile, Position>()) {
        auto& projPos = reg.get<Position>(proj);
        auto& projDmg = reg.get<Projectile>(proj).damage;
        bool  hit     = false;

        for (auto enemy : reg.view<Enemy, Position, Health>()) {
            auto& ep = reg.get<Position>(enemy);
            float d2 = (projPos.x - ep.x) * (projPos.x - ep.x)
                     + (projPos.z - ep.z) * (projPos.z - ep.z);
            if (d2 < 0.8f * 0.8f) { // combined radii
                auto& eh = reg.get<Health>(enemy);
                eh.hp -= projDmg;
                if (eh.hp <= 0.f) deadEnemies.push_back(enemy);
                hit = true;
                break;
            }
        }
        if (hit) deadProj.push_back(proj);
    }

    // Enemy–Player collision (damage player, bounce enemy)
    for (auto enemy : reg.view<Enemy, Position>()) {
        auto& ep   = reg.get<Position>(enemy);
        auto& en   = reg.get<Enemy>(enemy);
        float d2   = (ep.x - pp.x) * (ep.x - pp.x) + (ep.z - pp.z) * (ep.z - pp.z);
        if (d2 < 1.0f * 1.0f) {
            en.attackTimer -= dt;
            if (en.attackTimer <= 0.f) {
                ph.hp -= en.damage;
                en.attackTimer = 1.f;
            }
        }
    }

    // Drop XP orbs for killed enemies
    for (auto e : deadEnemies) {
        if (!reg.valid(e)) continue;
        auto pos    = reg.get<Position>(e);
        auto xpVal  = reg.get<Enemy>(e).xpDrop;
        auto orb    = reg.create();
        reg.emplace<Position>(orb, pos);
        reg.emplace<Velocity>(orb);
        reg.emplace<XPOrb>(orb, XPOrb{ xpVal, 20.f });
        reg.emplace<RenderCircle>(orb, RenderCircle{ {0.f, 1.f, 0.2f, 1.f}, 0.25f });
        reg.destroy(e);
    }
    for (auto e : deadProj) if (reg.valid(e)) reg.destroy(e);
}

static void sXPOrb(World& w, float dt, GameCtx& ctx) {
    if (ctx.player == entt::null) return;
    auto& reg   = w.reg;
    auto& pp    = reg.get<Position>(ctx.player);
    auto& pl    = reg.get<Player>(ctx.player);
    const float pickupR2 = 2.5f * 2.5f;

    std::vector<entt::entity> collected;
    for (auto e : reg.view<XPOrb, Position>()) {
        auto& op = reg.get<Position>(e);
        auto& xp = reg.get<XPOrb>(e);
        xp.lifetime -= dt;
        float d2 = (op.x - pp.x) * (op.x - pp.x) + (op.z - pp.z) * (op.z - pp.z);
        if (d2 < pickupR2 || xp.lifetime <= 0.f) collected.push_back(e);
    }
    for (auto e : collected) {
        if (!reg.valid(e)) continue;
        if (w.reg.any_of<XPOrb>(e)) {
            pl.xp += reg.get<XPOrb>(e).value;
        }
        reg.destroy(e);
    }
    // Simple level-up: every 100 XP
    int newLevel = 1 + static_cast<int>(pl.xp / 100.f);
    if (newLevel > pl.level) {
        pl.level = newLevel;
        pl.fireRate = std::max(0.05f, 0.25f - (pl.level - 1) * 0.02f);
    }
}

static void sSpawn(World& w, float dt, GameCtx& ctx) {
    ctx.spawnTimer -= dt;
    if (ctx.spawnTimer > 0.f) return;

    int cur = static_cast<int>(w.reg.view<Enemy>().size());
    if (cur >= ctx.maxEnemies) { ctx.spawnTimer = 0.1f; return; }

    ctx.spawnTimer = ctx.spawnInterval;
    // Shrink interval over time (more pressure)
    ctx.spawnInterval = std::max(0.1f, ctx.spawnInterval - 0.001f);

    // Spawn in a ring around the player
    const auto& pp = (ctx.player != entt::null)
        ? w.reg.get<Position>(ctx.player)
        : Position{};

    const float spawnR = 40.f;
    float angle = randf(0.f, 6.2832f);
    float ex    = pp.x + spawnR * std::cosf(angle);
    float ez    = pp.z + spawnR * std::sinf(angle);

    auto e = w.reg.create();
    w.reg.emplace<Position>(e, Position{ ex, ez });
    w.reg.emplace<Velocity>(e);
    w.reg.emplace<Enemy>(e, Enemy{ randf(1.5f, 3.5f), 10.f, 5.f, 0.f });
    w.reg.emplace<Health>(e, Health{ 50.f, 50.f });
    w.reg.emplace<RenderCircle>(e, RenderCircle{ {0.9f, 0.2f, 0.2f, 1.f}, 0.5f });
}

// Gathers all renderable entities into ctx.instances for the GPU upload.
static void sRenderSync(World& w, float dt, GameCtx& ctx) {
    ctx.instances.clear();

    auto Append = [&](const Position& pos, const RenderCircle& rc) {
        InstanceData d{};
        float s = rc.radius * 2.f;
        // Scale and translate: cube sits at Y=0, height=s
        XMMATRIX world = XMMatrixScaling(s, s, s)
                       * XMMatrixTranslation(pos.x, 0.f, pos.z);
        XMStoreFloat4x4(&d.world, XMMatrixTranspose(world));
        d.color = rc.color;
        ctx.instances.push_back(d);
    };

    // Player first (blue)
    if (ctx.player != entt::null) {
        Append(w.reg.get<Position>(ctx.player),
               w.reg.get<RenderCircle>(ctx.player));
    }

    for (auto e : w.reg.view<RenderCircle, Position>()) {
        if (e == ctx.player) continue;
        Append(w.reg.get<Position>(e), w.reg.get<RenderCircle>(e));
    }

    ctx.entityCount = static_cast<int>(ctx.instances.size());
}

// ---- registration -----------------------------------------------------------

void RegisterSystems(SystemGraph& graph, GameCtx& ctx, const bool* keys) {
    graph.add("PlayerInput", [&ctx, keys](World& w, float dt) {
        sPlayerInput(w, dt, keys, ctx);
    });
    graph.add("EnemyAI", [&ctx](World& w, float dt) {
        sEnemyAI(w, dt, ctx);
    }, {"PlayerInput"});
    graph.add("Weapon", [&ctx](World& w, float dt) {
        sWeapon(w, dt, ctx);
    }, {"PlayerInput"});
    graph.add("Movement", [](World& w, float dt) {
        sMovement(w, dt);
    }, {"EnemyAI", "Weapon"});
    graph.add("Collision", [&ctx](World& w, float dt) {
        sCollision(w, dt, ctx);
    }, {"Movement"});
    graph.add("Projectile", [](World& w, float dt) {
        sProjectile(w, dt);
    }, {"Collision"});
    graph.add("XPOrb", [&ctx](World& w, float dt) {
        sXPOrb(w, dt, ctx);
    }, {"Collision"});
    graph.add("Spawn", [&ctx](World& w, float dt) {
        sSpawn(w, dt, ctx);
    }, {"Collision"});
    graph.add("RenderSync", [&ctx](World& w, float dt) {
        sRenderSync(w, dt, ctx);
    }, {"Projectile", "XPOrb", "Spawn"});
}

} // namespace VS
