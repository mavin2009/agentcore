#ifndef AGENTCORE_ASYNC_EXECUTOR_H
#define AGENTCORE_ASYNC_EXECUTOR_H

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace agentcore {

class AsyncTaskExecutor {
public:
    explicit AsyncTaskExecutor(std::size_t worker_count = 0, std::size_t max_queue_depth = 0);
    ~AsyncTaskExecutor();

    AsyncTaskExecutor(const AsyncTaskExecutor&) = delete;
    auto operator=(const AsyncTaskExecutor&) -> AsyncTaskExecutor& = delete;

    void enqueue(std::function<void()> job);
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] std::size_t queue_size() const noexcept;

private:
    void worker_loop();

    std::size_t max_queue_depth_{0};
    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> jobs_;
    mutable std::mutex mutex_;
    std::condition_variable job_cv_;
    std::condition_variable capacity_cv_;
    bool stopping_{false};
};

} // namespace agentcore

#endif // AGENTCORE_ASYNC_EXECUTOR_H
