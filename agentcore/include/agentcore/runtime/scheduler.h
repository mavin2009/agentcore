#ifndef AGENTCORE_SCHEDULER_H
#define AGENTCORE_SCHEDULER_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include "agentcore/core/types.h"
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentcore {

struct ScheduledTask {
    RunId run_id{0};
    NodeId node_id{0};
    uint32_t branch_id{0};
    uint64_t ready_at_ns{0};
};

struct ScheduledTaskKey {
    RunId run_id{0};
    NodeId node_id{0};
    uint32_t branch_id{0};

    friend bool operator==(const ScheduledTaskKey& left, const ScheduledTaskKey& right) noexcept {
        return left.run_id == right.run_id &&
            left.node_id == right.node_id &&
            left.branch_id == right.branch_id;
    }
};

enum class AsyncWaitKind : uint8_t {
    Tool,
    Model
};

struct AsyncWaitKey {
    AsyncWaitKind kind{AsyncWaitKind::Tool};
    uint64_t handle_id{0};

    [[nodiscard]] bool valid() const noexcept { return handle_id != 0U; }

    friend bool operator==(const AsyncWaitKey& left, const AsyncWaitKey& right) noexcept {
        return left.kind == right.kind && left.handle_id == right.handle_id;
    }
};

class WorkQueue {
public:
    void push(const ScheduledTask& task);
    [[nodiscard]] std::optional<ScheduledTask> pop_ready(uint64_t now_ns);
    [[nodiscard]] std::optional<ScheduledTask> pop_ready_for_run(RunId run_id, uint64_t now_ns);
    [[nodiscard]] std::vector<ScheduledTask> pop_ready_batch_for_run(
        RunId run_id,
        uint64_t now_ns,
        std::size_t max_tasks
    );
    [[nodiscard]] bool has_ready(uint64_t now_ns) const;
    [[nodiscard]] bool has_ready_for_run(RunId run_id, uint64_t now_ns) const;
    [[nodiscard]] bool has_tasks_for_run(RunId run_id) const;
    [[nodiscard]] std::size_t task_count_for_run(RunId run_id) const;
    void remove_run(RunId run_id);
    [[nodiscard]] std::vector<ScheduledTask> tasks_for_run(RunId run_id) const;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct ScheduledTaskOrder {
        [[nodiscard]] bool operator()(const ScheduledTask& left, const ScheduledTask& right) const noexcept;
    };

    using TaskSet = std::multiset<ScheduledTask, ScheduledTaskOrder>;
    using TaskIterator = TaskSet::iterator;

    struct TaskIteratorOrder {
        [[nodiscard]] bool operator()(const TaskIterator& left, const TaskIterator& right) const noexcept;
    };

    [[nodiscard]] static bool task_ready_before(const ScheduledTask& left, const ScheduledTask& right) noexcept;
    [[nodiscard]] std::optional<TaskIterator> find_ready_iterator(std::optional<RunId> run_id_filter, uint64_t now_ns);
    [[nodiscard]] std::optional<ScheduledTask> pop_iterator(TaskIterator iterator);
    void remove_run_locked(RunId run_id);

    mutable std::mutex mutex_;
    TaskSet tasks_;
    std::unordered_map<RunId, std::set<TaskIterator, TaskIteratorOrder>> run_index_;
    std::unordered_map<RunId, std::size_t> run_task_counts_;
};

class WorkerPool {
public:
    explicit WorkerPool(std::size_t worker_count = 0);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    auto operator=(const WorkerPool&) -> WorkerPool& = delete;

    void run_batch(const std::vector<std::function<void()>>& jobs);
    [[nodiscard]] std::size_t worker_count() const noexcept { return worker_count_; }

private:
    struct BatchState {
        std::size_t remaining{0};
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::exception_ptr> errors;
    };

    struct QueuedJob {
        std::function<void()> work;
        std::shared_ptr<BatchState> batch;
    };

    void worker_loop();

    std::size_t worker_count_{1};
    std::vector<std::thread> workers_;
    std::deque<QueuedJob> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_{false};
};

class AsyncCompletionQueue {
public:
    void register_waiter(const AsyncWaitKey& key, const ScheduledTask& task);
    void signal_completion(const AsyncWaitKey& key);
    [[nodiscard]] std::size_t promote_ready(WorkQueue& work_queue, uint64_t now_ns);
    void remove_waiters_for_task(const ScheduledTask& task);
    void remove_run(RunId run_id);
    [[nodiscard]] bool has_waiters_for_run(RunId run_id) const;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    void wait_for_activity(std::chrono::milliseconds timeout);

private:
    struct AsyncWaitKeyHash {
        [[nodiscard]] std::size_t operator()(const AsyncWaitKey& key) const noexcept;
    };

    struct ScheduledTaskKeyHash {
        [[nodiscard]] std::size_t operator()(const ScheduledTaskKey& key) const noexcept;
    };

    [[nodiscard]] static ScheduledTaskKey task_key_for(const ScheduledTask& task) noexcept;
    void enqueue_ready_task_locked(const ScheduledTask& task);
    void erase_waiter_registration_locked(const AsyncWaitKey& key, const ScheduledTask& task);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<AsyncWaitKey, std::vector<ScheduledTask>, AsyncWaitKeyHash> waiters_;
    std::unordered_map<RunId, std::size_t> waiters_by_run_;
    std::unordered_map<ScheduledTaskKey, std::vector<AsyncWaitKey>, ScheduledTaskKeyHash> wait_keys_by_task_;
    std::unordered_set<AsyncWaitKey, AsyncWaitKeyHash> completed_without_waiters_;
    std::unordered_set<ScheduledTaskKey, ScheduledTaskKeyHash> signaled_task_keys_;
    std::deque<ScheduledTask> ready_tasks_;
    std::size_t waiter_count_{0};
};

class Scheduler {
public:
    explicit Scheduler(std::size_t worker_count = 0);

    void enqueue_task(const ScheduledTask& task);
    [[nodiscard]] std::optional<ScheduledTask> dequeue_ready(uint64_t now_ns);
    [[nodiscard]] std::optional<ScheduledTask> dequeue_ready_for_run(RunId run_id, uint64_t now_ns);
    [[nodiscard]] std::vector<ScheduledTask> dequeue_ready_batch_for_run(
        RunId run_id,
        uint64_t now_ns,
        std::size_t max_tasks
    );
    [[nodiscard]] bool has_ready(uint64_t now_ns) const;
    [[nodiscard]] bool has_ready_for_run(RunId run_id, uint64_t now_ns) const;
    [[nodiscard]] bool has_tasks_for_run(RunId run_id) const;
    [[nodiscard]] std::size_t task_count_for_run(RunId run_id) const;
    void remove_run(RunId run_id);
    [[nodiscard]] std::vector<ScheduledTask> tasks_for_run(RunId run_id) const;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t queue_size() const noexcept;
    [[nodiscard]] std::size_t parallelism() const noexcept;
    void run_batch(const std::vector<std::function<void()>>& jobs);
    void register_async_waiter(const AsyncWaitKey& key, const ScheduledTask& task);
    void signal_async_completion(const AsyncWaitKey& key);
    [[nodiscard]] std::size_t promote_ready_async_tasks(uint64_t now_ns);
    void remove_async_waiters_for_task(const ScheduledTask& task);
    [[nodiscard]] bool has_async_waiters_for_run(RunId run_id) const;
    [[nodiscard]] bool has_async_waiters() const noexcept;
    void wait_for_async_activity(std::chrono::milliseconds timeout);

private:
    WorkQueue work_queue_;
    WorkerPool worker_pool_;
    AsyncCompletionQueue async_completion_queue_;
};

} // namespace agentcore

#endif // AGENTCORE_SCHEDULER_H
