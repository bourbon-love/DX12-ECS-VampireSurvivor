#pragma once
#include <functional>
#include <string>
#include <vector>

namespace VS {

struct World;
using SystemFn = std::function<void(World&, float /*dt*/)>;

// Topological-sort–ordered system executor.
// add()  registers a system with optional "must run after X" constraints.
// run()  executes all systems in dependency order every frame.
class SystemGraph {
public:
    void add(std::string name, SystemFn fn, std::vector<std::string> after = {});
    void run(World& world, float dt);

private:
    struct Node {
        std::string             name;
        SystemFn                fn;
        std::vector<std::string> after;
    };

    std::vector<Node> m_nodes;
    std::vector<int>  m_order;
    bool              m_dirty = true;

    void sort();
};

} // namespace VS
