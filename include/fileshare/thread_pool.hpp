#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace fileshare {

// Fixed-size worker pool consuming tasks from a queue -- the "parallel task
// queue" pattern reused from the ATM project. The epoll reactor (M4) offloads
// per-connection I/O onto this pool so the epoll_wait thread never blocks.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t workers);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Enqueue a task. Ignored once stop() has begun.
    void submit(std::function<void()> task);

    // Stop accepting tasks, let workers drain the queue, then join them.
    // Idempotent; also called by the destructor.
    void stop();

    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }

private:
    void worker_loop();

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mutex_;
    std::condition_variable           cv_;
    bool                              stopping_ = false;
};

} // namespace fileshare
