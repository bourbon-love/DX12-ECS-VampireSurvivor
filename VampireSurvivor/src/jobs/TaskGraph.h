#pragma once
#include <functional>
#include <string>
#include <vector>

namespace VS {
class ThreadPool;

// Layer-based parallel task scheduler.
// Tasks in the same layer (no unresolved deps between them) run in parallel.
// Layers are synchronized with ThreadPool::waitIdle() between them.
//
// Usage:
//   TaskGraph g;
//   auto A = g.add("A", fnA);
//   auto B = g.add("B", fnB);
//   auto C = g.add("C", fnC);
//   g.depend(C, A);   // C runs after A
//   g.depend(C, B);   // C runs after B
//   g.run(pool);      // A and B run in parallel; then C runs
//   g.reset();
class TaskGraph {
public:
    struct Handle { size_t idx; };

    Handle add(std::string name, std::function<void()> fn);
    void   depend(Handle dependent, Handle dependency);
    void   run(ThreadPool& pool);
    void   reset();

private:
    struct Task {
        std::string            name;
        std::function<void()>  fn;
        std::vector<size_t>    deps;
    };

    std::vector<Task> m_tasks;
};

} // namespace VS
