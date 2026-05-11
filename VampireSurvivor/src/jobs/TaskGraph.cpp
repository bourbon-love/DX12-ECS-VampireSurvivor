#include "TaskGraph.h"
#include "ThreadPool.h"
#include <algorithm>
#include <future>
#include <stdexcept>

namespace VS {

TaskGraph::Handle TaskGraph::add(std::string name, std::function<void()> fn) {
    size_t idx = m_tasks.size();
    m_tasks.push_back({ std::move(name), std::move(fn), {} });
    return { idx };
}

void TaskGraph::depend(Handle dependent, Handle dependency) {
    m_tasks[dependent.idx].deps.push_back(dependency.idx);
}

void TaskGraph::run(ThreadPool& pool) {
    const size_t n = m_tasks.size();

    // Assign each task to a layer: layer = 1 + max(layer of deps), 0 if no deps.
    std::vector<int> layer(n, -1);
    std::function<int(size_t)> calcLayer = [&](size_t i) -> int {
        if (layer[i] >= 0) return layer[i];
        int maxDep = -1;
        for (size_t d : m_tasks[i].deps)
            maxDep = std::max(maxDep, calcLayer(d));
        return layer[i] = maxDep + 1;
    };
    for (size_t i = 0; i < n; ++i) calcLayer(i);

    int maxLayer = *std::max_element(layer.begin(), layer.end());

    // Execute layer by layer; tasks within a layer run in parallel.
    for (int L = 0; L <= maxLayer; ++L) {
        std::vector<std::future<void>> futs;
        for (size_t i = 0; i < n; ++i) {
            if (layer[i] == L)
                futs.push_back(pool.submit(m_tasks[i].fn));
        }
        for (auto& f : futs) f.get();
    }
}

void TaskGraph::reset() {
    m_tasks.clear();
}

} // namespace VS
