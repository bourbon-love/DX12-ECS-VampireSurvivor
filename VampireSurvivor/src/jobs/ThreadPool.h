#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace VS {

// Fixed-size worker thread pool.
// submit()   enqueues a callable and returns a std::future for the result.
// waitIdle() blocks until all enqueued tasks have completed.
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    void   waitIdle();
    size_t size() const { return m_workers.size(); }

private:
    std::vector<std::thread>          m_workers;
    std::queue<std::function<void()>> m_queue;
    std::mutex                        m_mutex;
    std::condition_variable           m_workCv;
    std::condition_variable           m_idleCv;
    int                               m_busy = 0;
    bool                              m_stop = false;
};

// ---- template implementation ----

template<typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>>
{
    using R = std::invoke_result_t<F, Args...>;
    auto task = std::make_shared<std::packaged_task<R()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    auto fut = task->get_future();
    {
        std::unique_lock lock(m_mutex);
        m_queue.push([task]() { (*task)(); });
        ++m_busy;
    }
    m_workCv.notify_one();
    return fut;
}

} // namespace VS
