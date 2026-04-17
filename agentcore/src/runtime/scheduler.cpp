#include "agentcore/runtime/scheduler.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>

namespace agentcore {

bool WorkQueue::task_ready_before(const ScheduledTask& left, const ScheduledTask& right) noexcept {
    if (left.ready_at_ns != right.ready_at_ns) {
        return left.ready_at_ns < right.ready_at_ns;
    }
    if (left.run_id != right.run_id) {
        return left.run_id < right.run_id;
    }
    if (left.branch_id != right.branch_id) {
        return left.branch_id < right.branch_id;
    }
    return left.node_id < right.node_id;
}

bool WorkQueue::ScheduledTaskOrder::operator()(const ScheduledTask& left, const ScheduledTask& right) const noexcept {
    return WorkQueue::task_ready_before(left, right);
}

bool WorkQueue::TaskIteratorOrder::operator()(const TaskIterator& left, const TaskIterator& right) const noexcept {
    if (WorkQueue::task_ready_before(*left, *right)) {
        return true;
    }
    if (WorkQueue::task_ready_before(*right, *left)) {
        return false;
    }
    return std::less<const ScheduledTask*>{}(std::addressof(*left), std::addressof(*right));
}

void WorkQueue::push(const ScheduledTask& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    const TaskIterator iterator = tasks_.insert(task);
    run_index_[task.run_id].insert(iterator);
    run_task_counts_[task.run_id] += 1U;
}

std::optional<WorkQueue::TaskIterator> WorkQueue::find_ready_iterator(
    std::optional<RunId> run_id_filter,
    uint64_t now_ns
) {
    if (run_id_filter.has_value()) {
        const auto run_iterator = run_index_.find(*run_id_filter);
        if (run_iterator == run_index_.end() || run_iterator->second.empty()) {
            return std::nullopt;
        }
        const TaskIterator iterator = *run_iterator->second.begin();
        if (iterator->ready_at_ns > now_ns) {
            return std::nullopt;
        }
        return iterator;
    }

    if (tasks_.empty()) {
        return std::nullopt;
    }
    const TaskIterator iterator = tasks_.begin();
    if (iterator->ready_at_ns > now_ns) {
        return std::nullopt;
    }
    return iterator;
}

std::optional<ScheduledTask> WorkQueue::pop_iterator(TaskIterator iterator) {
    if (iterator == tasks_.end()) {
        return std::nullopt;
    }

    const ScheduledTask task = *iterator;
    auto run_iterator = run_index_.find(task.run_id);
    if (run_iterator != run_index_.end()) {
        run_iterator->second.erase(iterator);
        if (run_iterator->second.empty()) {
            run_index_.erase(run_iterator);
        }
    }
    auto task_count_iterator = run_task_counts_.find(task.run_id);
    if (task_count_iterator != run_task_counts_.end()) {
        task_count_iterator->second -= 1U;
        if (task_count_iterator->second == 0U) {
            run_task_counts_.erase(task_count_iterator);
        }
    }
    tasks_.erase(iterator);
    return task;
}

std::optional<ScheduledTask> WorkQueue::pop_ready(uint64_t now_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::optional<TaskIterator> iterator = find_ready_iterator(std::nullopt, now_ns);
    if (!iterator.has_value()) {
        return std::nullopt;
    }
    return pop_iterator(*iterator);
}

std::optional<ScheduledTask> WorkQueue::pop_ready_for_run(RunId run_id, uint64_t now_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::optional<TaskIterator> iterator = find_ready_iterator(run_id, now_ns);
    if (!iterator.has_value()) {
        return std::nullopt;
    }
    return pop_iterator(*iterator);
}

std::vector<ScheduledTask> WorkQueue::pop_ready_batch_for_run(
    RunId run_id,
    uint64_t now_ns,
    std::size_t max_tasks
) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ScheduledTask> tasks;
    const auto run_iterator = run_index_.find(run_id);
    if (run_iterator == run_index_.end() || max_tasks == 0U) {
        return tasks;
    }

    std::vector<TaskIterator> selected;
    selected.reserve(std::min(max_tasks, run_iterator->second.size()));
    for (TaskIterator iterator : run_iterator->second) {
        if (iterator->ready_at_ns > now_ns || selected.size() >= max_tasks) {
            break;
        }
        selected.push_back(iterator);
    }

    tasks.reserve(selected.size());
    for (TaskIterator iterator : selected) {
        tasks.push_back(*iterator);
    }
    for (TaskIterator iterator : selected) {
        static_cast<void>(pop_iterator(iterator));
    }

    return tasks;
}

bool WorkQueue::has_ready(uint64_t now_ns) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !tasks_.empty() && tasks_.begin()->ready_at_ns <= now_ns;
}

bool WorkQueue::has_ready_for_run(RunId run_id, uint64_t now_ns) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto run_iterator = run_index_.find(run_id);
    return run_iterator != run_index_.end() &&
        !run_iterator->second.empty() &&
        (*run_iterator->second.begin())->ready_at_ns <= now_ns;
}

bool WorkQueue::has_tasks_for_run(RunId run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = run_task_counts_.find(run_id);
    return iterator != run_task_counts_.end() && iterator->second != 0U;
}

std::size_t WorkQueue::task_count_for_run(RunId run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = run_task_counts_.find(run_id);
    return iterator == run_task_counts_.end() ? 0U : iterator->second;
}

void WorkQueue::remove_run_locked(RunId run_id) {
    const auto run_iterator = run_index_.find(run_id);
    if (run_iterator == run_index_.end()) {
        run_task_counts_.erase(run_id);
        return;
    }

    std::vector<TaskIterator> to_remove;
    to_remove.reserve(run_iterator->second.size());
    for (TaskIterator iterator : run_iterator->second) {
        to_remove.push_back(iterator);
    }
    run_index_.erase(run_iterator);
    run_task_counts_.erase(run_id);
    for (TaskIterator iterator : to_remove) {
        tasks_.erase(iterator);
    }
}

void WorkQueue::remove_run(RunId run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    remove_run_locked(run_id);
}

std::vector<ScheduledTask> WorkQueue::tasks_for_run(RunId run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ScheduledTask> tasks;
    const auto run_iterator = run_index_.find(run_id);
    if (run_iterator == run_index_.end()) {
        return tasks;
    }
    tasks.reserve(run_iterator->second.size());
    for (TaskIterator iterator : run_iterator->second) {
        tasks.push_back(*iterator);
    }
    return tasks;
}

bool WorkQueue::empty() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.empty();
}

std::size_t WorkQueue::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

WorkerPool::WorkerPool(std::size_t worker_count) {
    const std::size_t detected_workers = worker_count == 0U
        ? std::max<std::size_t>(2U, std::thread::hardware_concurrency() == 0U ? 2U : std::thread::hardware_concurrency())
        : worker_count;
    worker_count_ = std::max<std::size_t>(1U, detected_workers);
    workers_.reserve(worker_count_);
    for (std::size_t index = 0; index < worker_count_; ++index) {
        workers_.push_back(std::thread([this]() {
            worker_loop();
        }));
    }
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void WorkerPool::run_batch(const std::vector<std::function<void()>>& jobs) {
    if (jobs.empty()) {
        return;
    }

    const auto batch = std::make_shared<BatchState>();
    batch->remaining = jobs.size();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& job : jobs) {
            jobs_.push_back(QueuedJob{job, batch});
        }
    }
    cv_.notify_all();

    std::unique_lock<std::mutex> batch_lock(batch->mutex);
    batch->cv.wait(batch_lock, [&batch]() {
        return batch->remaining == 0U;
    });

    if (!batch->errors.empty()) {
        std::rethrow_exception(batch->errors.front());
    }
}

void WorkerPool::worker_loop() {
    while (true) {
        QueuedJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stopping_ || !jobs_.empty();
            });
            if (stopping_ && jobs_.empty()) {
                return;
            }

            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        try {
            job.work();
        } catch (...) {
            std::lock_guard<std::mutex> batch_lock(job.batch->mutex);
            job.batch->errors.push_back(std::current_exception());
        }

        {
            std::lock_guard<std::mutex> batch_lock(job.batch->mutex);
            job.batch->remaining -= 1U;
            if (job.batch->remaining == 0U) {
                job.batch->cv.notify_all();
            }
        }
    }
}

std::size_t AsyncCompletionQueue::AsyncWaitKeyHash::operator()(const AsyncWaitKey& key) const noexcept {
    const std::size_t kind_hash = static_cast<std::size_t>(key.kind);
    const std::size_t handle_hash = std::hash<uint64_t>{}(key.handle_id);
    return handle_hash ^ (kind_hash + 0x9e3779b97f4a7c15ULL + (handle_hash << 6U) + (handle_hash >> 2U));
}

std::size_t AsyncCompletionQueue::ScheduledTaskKeyHash::operator()(const ScheduledTaskKey& key) const noexcept {
    const std::size_t run_hash = std::hash<RunId>{}(key.run_id);
    const std::size_t node_hash = std::hash<NodeId>{}(key.node_id);
    const std::size_t branch_hash = std::hash<uint32_t>{}(key.branch_id);
    return run_hash ^
        (node_hash + 0x9e3779b97f4a7c15ULL + (run_hash << 6U) + (run_hash >> 2U)) ^
        (branch_hash + 0x9e3779b97f4a7c15ULL + (node_hash << 6U) + (node_hash >> 2U));
}

ScheduledTaskKey AsyncCompletionQueue::task_key_for(const ScheduledTask& task) noexcept {
    return ScheduledTaskKey{task.run_id, task.node_id, task.branch_id};
}

void AsyncCompletionQueue::enqueue_ready_task_locked(const ScheduledTask& task) {
    if (signaled_task_keys_.insert(task_key_for(task)).second) {
        ready_tasks_.push_back(task);
    }
}

void AsyncCompletionQueue::erase_waiter_registration_locked(const AsyncWaitKey& key, const ScheduledTask& task) {
    const auto waiters_iterator = waiters_.find(key);
    if (waiters_iterator == waiters_.end()) {
        return;
    }

    std::vector<ScheduledTask>& waiters = waiters_iterator->second;
    const auto erase_begin = std::remove_if(waiters.begin(), waiters.end(), [&task](const ScheduledTask& candidate) {
        return candidate.run_id == task.run_id &&
            candidate.node_id == task.node_id &&
            candidate.branch_id == task.branch_id;
    });
    const std::size_t removed = static_cast<std::size_t>(std::distance(erase_begin, waiters.end()));
    if (removed != 0U) {
        waiters.erase(erase_begin, waiters.end());
        auto run_iterator = waiters_by_run_.find(task.run_id);
        if (run_iterator != waiters_by_run_.end()) {
            run_iterator->second -= removed;
            if (run_iterator->second == 0U) {
                waiters_by_run_.erase(run_iterator);
            }
        }
        waiter_count_ -= removed;
    }
    if (waiters.empty()) {
        waiters_.erase(waiters_iterator);
    }
}

void AsyncCompletionQueue::register_waiter(const AsyncWaitKey& key, const ScheduledTask& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!key.valid()) {
        return;
    }

    const ScheduledTaskKey task_key = task_key_for(task);
    std::vector<AsyncWaitKey>& task_wait_keys = wait_keys_by_task_[task_key];
    if (std::find(task_wait_keys.begin(), task_wait_keys.end(), key) != task_wait_keys.end()) {
        return;
    }

    const auto completed_iterator = completed_without_waiters_.find(key);
    if (completed_iterator != completed_without_waiters_.end()) {
        completed_without_waiters_.erase(completed_iterator);
        enqueue_ready_task_locked(task);
        cv_.notify_all();
        return;
    }

    waiters_[key].push_back(task);
    task_wait_keys.push_back(key);
    waiters_by_run_[task.run_id] += 1U;
    waiter_count_ += 1U;
}

void AsyncCompletionQueue::signal_completion(const AsyncWaitKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!key.valid()) {
        return;
    }

    const auto waiters_iterator = waiters_.find(key);
    if (waiters_iterator == waiters_.end()) {
        completed_without_waiters_.insert(key);
        cv_.notify_all();
        return;
    }

    const std::vector<ScheduledTask> waiters = waiters_iterator->second;
    waiters_.erase(waiters_iterator);
    for (const ScheduledTask& task : waiters) {
        const ScheduledTaskKey task_key = task_key_for(task);
        auto task_wait_keys_iterator = wait_keys_by_task_.find(task_key);
        if (task_wait_keys_iterator != wait_keys_by_task_.end()) {
            std::vector<AsyncWaitKey>& task_wait_keys = task_wait_keys_iterator->second;
            task_wait_keys.erase(
                std::remove(task_wait_keys.begin(), task_wait_keys.end(), key),
                task_wait_keys.end()
            );
            if (task_wait_keys.empty()) {
                wait_keys_by_task_.erase(task_wait_keys_iterator);
            }
        }

        auto run_iterator = waiters_by_run_.find(task.run_id);
        if (run_iterator != waiters_by_run_.end()) {
            run_iterator->second -= 1U;
            if (run_iterator->second == 0U) {
                waiters_by_run_.erase(run_iterator);
            }
        }
        waiter_count_ -= 1U;
        enqueue_ready_task_locked(task);
    }
    cv_.notify_all();
}

std::size_t AsyncCompletionQueue::promote_ready(WorkQueue& work_queue, uint64_t now_ns) {
    std::deque<ScheduledTask> ready_tasks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_tasks.swap(ready_tasks_);
    }

    std::size_t promoted = 0;
    while (!ready_tasks.empty()) {
        ScheduledTask ready_task = ready_tasks.front();
        ready_tasks.pop_front();
        ready_task.ready_at_ns = now_ns;
        work_queue.push(ready_task);
        promoted += 1U;
    }
    return promoted;
}

void AsyncCompletionQueue::remove_run(RunId run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto iterator = waiters_.begin(); iterator != waiters_.end();) {
        std::vector<ScheduledTask>& waiters = iterator->second;
        const auto erase_begin = std::remove_if(waiters.begin(), waiters.end(), [run_id](const ScheduledTask& task) {
            return task.run_id == run_id;
        });
        const std::size_t removed = static_cast<std::size_t>(std::distance(erase_begin, waiters.end()));
        if (removed != 0U) {
            waiter_count_ -= removed;
            waiters.erase(erase_begin, waiters.end());
        }
        if (waiters.empty()) {
            iterator = waiters_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    waiters_by_run_.erase(run_id);
    ready_tasks_.erase(
        std::remove_if(ready_tasks_.begin(), ready_tasks_.end(), [run_id](const ScheduledTask& task) {
            return task.run_id == run_id;
        }),
        ready_tasks_.end()
    );
    for (auto iterator = wait_keys_by_task_.begin(); iterator != wait_keys_by_task_.end();) {
        if (iterator->first.run_id == run_id) {
            iterator = wait_keys_by_task_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = signaled_task_keys_.begin(); iterator != signaled_task_keys_.end();) {
        if (iterator->run_id == run_id) {
            iterator = signaled_task_keys_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    cv_.notify_all();
}

void AsyncCompletionQueue::remove_waiters_for_task(const ScheduledTask& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ScheduledTaskKey task_key = task_key_for(task);

    const auto task_wait_keys_iterator = wait_keys_by_task_.find(task_key);
    if (task_wait_keys_iterator != wait_keys_by_task_.end()) {
        const std::vector<AsyncWaitKey> wait_keys = task_wait_keys_iterator->second;
        for (const AsyncWaitKey& key : wait_keys) {
            erase_waiter_registration_locked(key, task);
        }
        wait_keys_by_task_.erase(task_wait_keys_iterator);
    }

    ready_tasks_.erase(
        std::remove_if(ready_tasks_.begin(), ready_tasks_.end(), [&task](const ScheduledTask& candidate) {
            return candidate.run_id == task.run_id &&
                candidate.node_id == task.node_id &&
                candidate.branch_id == task.branch_id;
        }),
        ready_tasks_.end()
    );
    signaled_task_keys_.erase(task_key);
    cv_.notify_all();
}

bool AsyncCompletionQueue::has_waiters_for_run(RunId run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = waiters_by_run_.find(run_id);
    return iterator != waiters_by_run_.end() && iterator->second != 0U;
}

bool AsyncCompletionQueue::empty() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiter_count_ == 0U && ready_tasks_.empty();
}

std::size_t AsyncCompletionQueue::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiter_count_ + ready_tasks_.size();
}

void AsyncCompletionQueue::wait_for_activity(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this]() {
        return !ready_tasks_.empty();
    });
}

Scheduler::Scheduler(std::size_t worker_count) : worker_pool_(worker_count) {}

void Scheduler::enqueue_task(const ScheduledTask& task) {
    work_queue_.push(task);
}

std::optional<ScheduledTask> Scheduler::dequeue_ready(uint64_t now_ns) {
    return work_queue_.pop_ready(now_ns);
}

std::optional<ScheduledTask> Scheduler::dequeue_ready_for_run(RunId run_id, uint64_t now_ns) {
    return work_queue_.pop_ready_for_run(run_id, now_ns);
}

std::vector<ScheduledTask> Scheduler::dequeue_ready_batch_for_run(
    RunId run_id,
    uint64_t now_ns,
    std::size_t max_tasks
) {
    return work_queue_.pop_ready_batch_for_run(run_id, now_ns, max_tasks);
}

bool Scheduler::has_ready(uint64_t now_ns) const {
    return work_queue_.has_ready(now_ns);
}

bool Scheduler::has_ready_for_run(RunId run_id, uint64_t now_ns) const {
    return work_queue_.has_ready_for_run(run_id, now_ns);
}

bool Scheduler::has_tasks_for_run(RunId run_id) const {
    return work_queue_.has_tasks_for_run(run_id);
}

std::size_t Scheduler::task_count_for_run(RunId run_id) const {
    return work_queue_.task_count_for_run(run_id);
}

void Scheduler::remove_run(RunId run_id) {
    work_queue_.remove_run(run_id);
    async_completion_queue_.remove_run(run_id);
}

std::vector<ScheduledTask> Scheduler::tasks_for_run(RunId run_id) const {
    return work_queue_.tasks_for_run(run_id);
}

bool Scheduler::empty() const noexcept {
    return work_queue_.empty() && async_completion_queue_.empty();
}

std::size_t Scheduler::queue_size() const noexcept {
    return work_queue_.size() + async_completion_queue_.size();
}

std::size_t Scheduler::parallelism() const noexcept {
    return worker_pool_.worker_count();
}

void Scheduler::run_batch(const std::vector<std::function<void()>>& jobs) {
    worker_pool_.run_batch(jobs);
}

void Scheduler::register_async_waiter(const AsyncWaitKey& key, const ScheduledTask& task) {
    async_completion_queue_.register_waiter(key, task);
}

void Scheduler::signal_async_completion(const AsyncWaitKey& key) {
    async_completion_queue_.signal_completion(key);
}

std::size_t Scheduler::promote_ready_async_tasks(uint64_t now_ns) {
    return async_completion_queue_.promote_ready(work_queue_, now_ns);
}

void Scheduler::remove_async_waiters_for_task(const ScheduledTask& task) {
    async_completion_queue_.remove_waiters_for_task(task);
}

bool Scheduler::has_async_waiters_for_run(RunId run_id) const {
    return async_completion_queue_.has_waiters_for_run(run_id);
}

bool Scheduler::has_async_waiters() const noexcept {
    return !async_completion_queue_.empty();
}

void Scheduler::wait_for_async_activity(std::chrono::milliseconds timeout) {
    async_completion_queue_.wait_for_activity(timeout);
}

} // namespace agentcore
