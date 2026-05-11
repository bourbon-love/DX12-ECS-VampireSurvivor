#include "ThreadPool.h"
#include <algorithm>

namespace VS {

ThreadPool::ThreadPool(size_t numThreads) {
    if (numThreads == 0)
        numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);

    for (size_t i = 0; i < numThreads; ++i) {
        m_workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock lock(m_mutex);
                    m_workCv.wait(lock, [this] {
                        return m_stop || !m_queue.empty();
                    });
                    if (m_stop && m_queue.empty()) return;
                    task = std::move(m_queue.front());
                    m_queue.pop();
                }
                task();
                {
                    std::unique_lock lock(m_mutex);
                    if (--m_busy == 0 && m_queue.empty())
                        m_idleCv.notify_all();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock lock(m_mutex);
        m_stop = true;
    }
    m_workCv.notify_all();
    for (auto& t : m_workers) t.join();
}

void ThreadPool::waitIdle() {
    std::unique_lock lock(m_mutex);
    m_idleCv.wait(lock, [this] {
        return m_busy == 0 && m_queue.empty();
    });
}

} // namespace VS
