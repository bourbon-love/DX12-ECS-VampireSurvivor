#include "SystemGraph.h"
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace VS {

void SystemGraph::add(std::string name, SystemFn fn, std::vector<std::string> after) {
    m_nodes.push_back({ std::move(name), std::move(fn), std::move(after) });
    m_dirty = true;
}

void SystemGraph::sort() {
    const int n = static_cast<int>(m_nodes.size());
    std::unordered_map<std::string, int> idx;
    for (int i = 0; i < n; ++i) idx[m_nodes[i].name] = i;

    std::vector<int> inDeg(n, 0);
    std::vector<std::vector<int>> adj(n);

    for (int i = 0; i < n; ++i) {
        for (const auto& dep : m_nodes[i].after) {
            int j = idx.at(dep);
            adj[j].push_back(i);
            ++inDeg[i];
        }
    }

    std::queue<int> q;
    for (int i = 0; i < n; ++i)
        if (inDeg[i] == 0) q.push(i);

    m_order.clear();
    while (!q.empty()) {
        int v = q.front(); q.pop();
        m_order.push_back(v);
        for (int u : adj[v])
            if (--inDeg[u] == 0) q.push(u);
    }

    if (static_cast<int>(m_order.size()) != n)
        throw std::runtime_error("SystemGraph: dependency cycle detected");

    m_dirty = false;
}

void SystemGraph::run(World& world, float dt) {
    if (m_dirty) sort();
    for (int i : m_order)
        m_nodes[i].fn(world, dt);
}

} // namespace VS
