#pragma once
#include <entt/entt.hpp>

namespace VS {

// Thin wrapper — keeps GameApp from depending directly on entt types.
struct World {
    entt::registry reg;
};

} // namespace VS
