#pragma once
#include <DirectXMath.h>
#include <cstdint>

namespace VS {

struct Position { float x = 0.f, z = 0.f; };
struct Velocity { float vx = 0.f, vz = 0.f; };

struct Health {
    float hp    = 100.f;
    float maxHp = 100.f;
};

struct RenderCircle {
    DirectX::XMFLOAT4 color{ 1.f, 1.f, 1.f, 1.f };
    float radius = 0.5f;
};

// Tag + state components
struct Player {
    float speed         = 7.f;
    float fireRate      = 0.25f;   // seconds between shots
    float fireCooldown  = 0.f;
    float xp            = 0.f;
    int   level         = 1;
};

struct Enemy {
    float speed        = 2.f;
    float damage       = 10.f;
    float xpDrop       = 5.f;
    float attackTimer  = 0.f;
};

struct Projectile {
    float damage   = 25.f;
    float lifetime = 3.f;
};

struct XPOrb {
    float value    = 5.f;
    float lifetime = 20.f;
};

} // namespace VS
