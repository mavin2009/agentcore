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

bool WorkQueue::delayed_task_later(const ScheduledTask& left, const ScheduledTask& right) noexcept {
    return task_ready_before(right, left);
}

bool WorkQueue::ReadyRunFrontOrder::operator()(const ReadyRunFront& left, const ReadyRunFront& right) const noexcept {
    if (WorkQueue::task_ready_before(left.task, right.task)) {
        return true;
    }
    if (WorkQueue::task_ready_before(right.task, left.task)) {
        return false;
    }
    return left.run_id < right.run_id;
}

void WorkQueue::promote_ready_locked(uint64_t now_ns) {
    last_observed_now_ns_ = std::max(last_observed_now_ns_, now_ns);
    while (!delayed_heap_.empty() && delayed_heap_.front().ready_at_ns <= last_observed_now_ns_) {
        std::pop_heap(delayed_heap_.begin(), delayed_heap_.end(), delayed_task_later);
        ScheduledTask task = delayed_heap_.back();
        delayed_heap_.pop_back();
        enqueue_ready_locked(task);
    }
}

void WorkQueue::refresh_ready_front_locked(RunId run_id) {
    const auto ready_front_iterator = ready_front_index_.find(run_id);
    if (ready_front_iterator != ready_front_index_.end()) {
        ready_fronts_.erase(ready_front_iterator->second);
        ready_front_index_.erase(ready_front_iterator);
    }

    auto run_iterator = ready_by_run_.find(run_id);
    if (run_iterator == ready_by_run_.end() || run_iterator->second.empty()) {
        if (run_iterator != ready_by_run_.end() && run_iterator->second.empty()) {
            ready_by_run_.erase(run_iterator);
        }
        return;
    }

    auto [inserted_iterator, inserted] = ready_fronts_.insert(ReadyRunFront{run_id, run_iterator->second.front()});
    (void)inserted;
    ready_front_index_[run_id] = inserted_iterator;
}

void WorkQueue::enqueue_ready_locked(const ScheduledTask& task) {
    std::deque<ScheduledTask>& ready_tasks = ready_by_run_[task.run_id];
    const auto insert_position = std::find_if(
        ready_tasks.begin(),
        ready_tasks.end(),
        [&task](const ScheduledTask& candidate) {
            return task_ready_before(task, candidate);
        }
    );
    ready_tasks.insert(insert_position, task);
    refresh_ready_front_locked(task.run_id);
}

void WorkQueue::push(const ScheduledTask& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    run_task_counts_[task.run_id] += 1U;
    total_task_count_ += 1U;
    if (task.ready_at_ns <= last_observed_now_ns_) {
        enqueue_ready_locked(task);
        return;
    }

    delayed_heap_.push_back(task);
    std::push_heap(delayed_heap_.begin(), delayed_heap_.end(), delayed_task_later);
}

std::optional<ScheduledTask> WorkQueue::pop_ready_locked(RunId run_id) {
    const auto run_iterator = ready_by_run_.find(run_id);
    if (run_iterator == ready_by_run_.end() || run_iterator->second.empty()) {
        return std::nullopt;
    }

    ScheduledTask task = run_iterator->second.front();
    run_iterator->second.pop_front();
    refresh_ready_front_locked(run_id);

    auto task_count_iterator = run_task_counts_.find(run_id);
    if (task_count_iterator != run_task_counts_.end()) {
        task_count_iterator->second -= 1U;
        if (task_count_iterator->second == 0U) {
            run_task_counts_.erase(task_count_iterator);
        }
    }
    total_task_count_ -= 1U;
    return task;
}

std::optional<ScheduledTask> WorkQueue::pop_ready(uint64_t now_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    promote_ready_locked(now_ns);
    if (ready_fronts_.empty()) {
        return std::nullopt;
    }
    return pop_ready_locked(ready_fronts_.begin()->run_id);
}

std::optional<ScheduledTask> WorkQueue::pop_ready_for_run(RunId run_id, uint64_t now_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    promote_ready_locked(now_ns);
    return pop_ready_locked(run_id);
}

std::vector<ScheduledTask> WorkQueue::pop_ready_batch_for_run(
    RunId run_id,
    uint64_t now_ns,
    std::size_t max_tasks
) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ScheduledTask> tasks;
    if (max_tasks == 0U) {
        return tasks;
    }

    promote_ready_locked(now_ns);
    const auto run_iterator = ready_by_run_.find(run_id);
    if (run_iterator == ready_by_run_.end()) {
        return tasks;
    }

    tasks.reserve(std::min(max_tasks, run_iterator->second.size()));
    while (tasks.size() < max_tasks) {
        const std::optional<ScheduledTask> task = pop_ready_locked(run_id);
        if (!task.has_value()) {
            break;
        }
        tasks.push_back(*task);
    }
    return tasks;
}

bool WorkQueue::has_ready(uint64_t now_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    promote_ready_locked(now_ns);
    return !ready_fronts_.empty();
}

bool WorkQueue::has_ready_for_run(RunId run_id, uint64_t now_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    promote_ready_locked(now_ns);
    const auto run_iterator = ready_by_run_.find(run_id);
    return run_iterator != ready_by_run_.end() && !run_iterator->second.empty();
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

std::optional<uint64_t> WorkQueue::next_task_ready_time_for_run_locked(RunId run_id) const {
    std::optional<uint64_t> next_ready_time;

    const auto ready_iterator = ready_by_run_.find(run_id);
    if (ready_iterator != ready_by_run_.end() && !ready_iterator->second.empty()) {
        next_ready_time = ready_iterator->second.front().ready_at_ns;
    }

    for (const ScheduledTask& task : delayed_heap_) {
        if (task.run_id != run_id) {
            continue;
        }
        if (!next_ready_time.has_value() || task.ready_at_ns < *next_ready_time) {
            next_ready_time = task.ready_at_ns;
        }
    }

    return next_ready_time;
}

std::optional<uint64_t> WorkQueue::next_task_ready_time_for_run(RunId run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_task_ready_time_for_run_locked(run_id);
}

void WorkQueue::remove_run_locked(RunId run_id) {
    const auto task_count_iterator = run_task_counts_.find(run_id);
    const std::size_t removed_task_count = task_count_iterator == run_task_counts_.end()
        ? 0U
        : task_count_iterator->second;

    const auto ready_front_iterator = ready_front_index_.find(run_id);
    if (ready_front_iterator != ready_front_index_.end()) {
        ready_fronts_.erase(ready_front_iterator->second);
        ready_front_index_.erase(ready_front_iterator);
    }

    ready_by_run_.erase(run_id);

    if (!delayed_heap_.empty()) {
        const auto erase_begin = std::remove_if(
            delayed_heap_.begin(),
            delayed_heap_.end(),
            [run_id](const ScheduledTask& task) {
                return task.run_id == run_id;
            }
        );
        if (erase_begin != delayed_heap_.end()) {
            delayed_heap_.erase(erase_begin, delayed_heap_.end());
            std::make_heap(delayed_heap_.begin(), delayed_heap_.end(), delayed_task_later);
        }
    }

    run_task_counts_.erase(run_id);
    total_task_count_ -= removed_task_count;
}

void WorkQueue::remove_run(RunId run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    remove_run_locked(run_id);
}

void WorkQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_by_run_.clear();
    delayed_heap_.clear();
    ready_fronts_.clear();
    ready_front_index_.clear();
    run_task_counts_.clear();
    total_task_count_ = 0U;
    last_observed_now_ns_ = 0U;
}

std::vector<ScheduledTask> WorkQueue::tasks_for_run(RunId run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ScheduledTask> tasks;
    const auto ready_iterator = ready_by_run_.find(run_id);
    if (ready_iterator != ready_by_run_.end()) {
        tasks.insert(tasks.end(), ready_iterator->second.begin(), ready_iterator->second.end());
    }
    for (const ScheduledTask& task : delayed_heap_) {
        if (task.run_id == run_id) {
            tasks.push_back(task);
        }
    }
    if (!tasks.empty()) {
        std::sort(tasks.begin(), tasks.end(), task_ready_before);
    }
    return tasks;
}

bool WorkQueue::empty() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_task_count_ == 0U;
}

std::size_t WorkQueue::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_task_count_;
}

WorkerPool::WorkerPool(std::size_t worker_count, bool inline_mode) {
    inline_mode_ = inline_mode;
    if (inline_mode_) {
        worker_count_ = 1U;
        return;
    }

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

    if (inline_mode_) {
        for (const auto& job : jobs) {
            job();
        }
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

void WorkerPool::run_async(std::function<void()> job) {
    if (!job) {
        return;
    }

    if (inline_mode_) {
        job();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(QueuedJob{std::move(job), nullptr});
    }
    cv_.notify_one();
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
            if (job.batch) {
                std::lock_guard<std::mutex> batch_lock(job.batch->mutex);
                job.batch->errors.push_back(std::current_exception());
            }
        }

        if (job.batch) {
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
        ready_tasks_by_run_[task.run_id] += 1U;
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

void AsyncCompletionQueue::erase_wait_group_locked(const ScheduledTaskKey& task_key) {
    const auto group_iterator = wait_groups_by_task_.find(task_key);
    if (group_iterator == wait_groups_by_task_.end()) {
        return;
    }

    const ScheduledTask task = group_iterator->second.task;
    const std::size_t removed = group_iterator->second.pending_keys.size();
    for (const AsyncWaitKey& key : group_iterator->second.pending_keys) {
        auto keyed_tasks_iterator = wait_group_tasks_by_key_.find(key);
        if (keyed_tasks_iterator == wait_group_tasks_by_key_.end()) {
            continue;
        }

        std::vector<ScheduledTaskKey>& task_keys = keyed_tasks_iterator->second;
        task_keys.erase(
            std::remove(task_keys.begin(), task_keys.end(), task_key),
            task_keys.end()
        );
        if (task_keys.empty()) {
            wait_group_tasks_by_key_.erase(keyed_tasks_iterator);
        }
    }

    wait_groups_by_task_.erase(group_iterator);
    if (removed != 0U) {
        auto run_iterator = waiters_by_run_.find(task.run_id);
        if (run_iterator != waiters_by_run_.end()) {
            run_iterator->second -= removed;
            if (run_iterator->second == 0U) {
                waiters_by_run_.erase(run_iterator);
            }
        }
        waiter_count_ -= removed;
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

void AsyncCompletionQueue::register_wait_group(
    const std::vector<AsyncWaitKey>& keys,
    const ScheduledTask& task
) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ScheduledTaskKey task_key = task_key_for(task);
    erase_wait_group_locked(task_key);

    std::vector<AsyncWaitKey> unique_keys;
    unique_keys.reserve(keys.size());
    for (const AsyncWaitKey& key : keys) {
        if (!key.valid() ||
            std::find(unique_keys.begin(), unique_keys.end(), key) != unique_keys.end()) {
            continue;
        }
        unique_keys.push_back(key);
    }

    if (unique_keys.empty()) {
        enqueue_ready_task_locked(task);
        cv_.notify_all();
        return;
    }

    WaitGroupState wait_group;
    wait_group.task = task;
    wait_group.pending_keys.reserve(unique_keys.size());
    for (const AsyncWaitKey& key : unique_keys) {
        const auto completed_iterator = completed_without_waiters_.find(key);
        if (completed_iterator != completed_without_waiters_.end()) {
            completed_without_waiters_.erase(completed_iterator);
            continue;
        }

        wait_group.pending_keys.push_back(key);
        wait_group_tasks_by_key_[key].push_back(task_key);
    }

    if (wait_group.pending_keys.empty()) {
        enqueue_ready_task_locked(task);
        cv_.notify_all();
        return;
    }

    waiters_by_run_[task.run_id] += wait_group.pending_keys.size();
    waiter_count_ += wait_group.pending_keys.size();
    wait_groups_by_task_[task_key] = std::move(wait_group);
}

void AsyncCompletionQueue::signal_completion(const AsyncWaitKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!key.valid()) {
        return;
    }

    const auto waiters_iterator = waiters_.find(key);
    const auto grouped_waiters_iterator = wait_group_tasks_by_key_.find(key);
    if (waiters_iterator == waiters_.end() && grouped_waiters_iterator == wait_group_tasks_by_key_.end()) {
        completed_without_waiters_.insert(key);
        cv_.notify_all();
        return;
    }

    if (waiters_iterator != waiters_.end()) {
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
    }

    if (grouped_waiters_iterator != wait_group_tasks_by_key_.end()) {
        const std::vector<ScheduledTaskKey> grouped_tasks = grouped_waiters_iterator->second;
        wait_group_tasks_by_key_.erase(grouped_waiters_iterator);
        for (const ScheduledTaskKey& task_key : grouped_tasks) {
            auto group_iterator = wait_groups_by_task_.find(task_key);
            if (group_iterator == wait_groups_by_task_.end()) {
                continue;
            }

            std::vector<AsyncWaitKey>& pending_keys = group_iterator->second.pending_keys;
            const auto erase_begin = std::remove(pending_keys.begin(), pending_keys.end(), key);
            const std::size_t removed = static_cast<std::size_t>(std::distance(erase_begin, pending_keys.end()));
            if (removed != 0U) {
                pending_keys.erase(erase_begin, pending_keys.end());
                auto run_iterator = waiters_by_run_.find(group_iterator->second.task.run_id);
                if (run_iterator != waiters_by_run_.end()) {
                    run_iterator->second -= removed;
                    if (run_iterator->second == 0U) {
                        waiters_by_run_.erase(run_iterator);
                    }
                }
                waiter_count_ -= removed;
            }

            if (pending_keys.empty()) {
                const ScheduledTask task = group_iterator->second.task;
                wait_groups_by_task_.erase(group_iterator);
                enqueue_ready_task_locked(task);
            }
        }
    }
    cv_.notify_all();
}

std::size_t AsyncCompletionQueue::promote_ready(WorkQueue& work_queue, uint64_t now_ns) {
    std::deque<ScheduledTask> ready_tasks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_tasks.swap(ready_tasks_);
        ready_tasks_by_run_.clear();
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
    ready_tasks_by_run_.erase(run_id);
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
    for (auto iterator = wait_groups_by_task_.begin(); iterator != wait_groups_by_task_.end();) {
        if (iterator->second.task.run_id != run_id) {
            ++iterator;
            continue;
        }

        for (const AsyncWaitKey& key : iterator->second.pending_keys) {
            auto keyed_tasks_iterator = wait_group_tasks_by_key_.find(key);
            if (keyed_tasks_iterator == wait_group_tasks_by_key_.end()) {
                continue;
            }
            std::vector<ScheduledTaskKey>& task_keys = keyed_tasks_iterator->second;
            task_keys.erase(
                std::remove(task_keys.begin(), task_keys.end(), iterator->first),
                task_keys.end()
            );
            if (task_keys.empty()) {
                wait_group_tasks_by_key_.erase(keyed_tasks_iterator);
            }
        }
        iterator = wait_groups_by_task_.erase(iterator);
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

void AsyncCompletionQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    waiters_.clear();
    wait_group_tasks_by_key_.clear();
    waiters_by_run_.clear();
    wait_keys_by_task_.clear();
    wait_groups_by_task_.clear();
    completed_without_waiters_.clear();
    signaled_task_keys_.clear();
    ready_tasks_.clear();
    ready_tasks_by_run_.clear();
    waiter_count_ = 0U;
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

    erase_wait_group_locked(task_key);

    const auto ready_erase_begin = std::remove_if(
        ready_tasks_.begin(),
        ready_tasks_.end(),
        [&task](const ScheduledTask& candidate) {
            return candidate.run_id == task.run_id &&
                candidate.node_id == task.node_id &&
                candidate.branch_id == task.branch_id;
        }
    );
    const std::size_t removed_ready_tasks = static_cast<std::size_t>(
        std::distance(ready_erase_begin, ready_tasks_.end())
    );
    ready_tasks_.erase(ready_erase_begin, ready_tasks_.end());
    if (removed_ready_tasks != 0U) {
        auto run_iterator = ready_tasks_by_run_.find(task.run_id);
        if (run_iterator != ready_tasks_by_run_.end()) {
            run_iterator->second -= removed_ready_tasks;
            if (run_iterator->second == 0U) {
                ready_tasks_by_run_.erase(run_iterator);
            }
        }
    }
    signaled_task_keys_.erase(task_key);
    cv_.notify_all();
}

bool AsyncCompletionQueue::has_waiters_for_run(RunId run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto waiters_iterator = waiters_by_run_.find(run_id);
    if (waiters_iterator != waiters_by_run_.end() && waiters_iterator->second != 0U) {
        return true;
    }
    const auto ready_iterator = ready_tasks_by_run_.find(run_id);
    return ready_iterator != ready_tasks_by_run_.end() && ready_iterator->second != 0U;
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

Scheduler::Scheduler(std::size_t worker_count, bool inline_worker_pool)
    : worker_pool_(worker_count, inline_worker_pool) {}

void Scheduler::enqueue_task(const ScheduledTask& task) {
    work_queue_.push(task);
    notify_activity();
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

bool Scheduler::has_ready(uint64_t now_ns) {
    return work_queue_.has_ready(now_ns);
}

bool Scheduler::has_ready_for_run(RunId run_id, uint64_t now_ns) {
    return work_queue_.has_ready_for_run(run_id, now_ns);
}

bool Scheduler::has_tasks_for_run(RunId run_id) const {
    return work_queue_.has_tasks_for_run(run_id);
}

std::size_t Scheduler::task_count_for_run(RunId run_id) const {
    return work_queue_.task_count_for_run(run_id);
}

std::optional<uint64_t> Scheduler::next_task_ready_time_for_run(RunId run_id) const {
    return work_queue_.next_task_ready_time_for_run(run_id);
}

void Scheduler::remove_run(RunId run_id) {
    work_queue_.remove_run(run_id);
    async_completion_queue_.remove_run(run_id);
    notify_activity();
}

void Scheduler::clear() {
    work_queue_.clear();
    async_completion_queue_.clear();
    notify_activity();
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

bool Scheduler::inline_mode() const noexcept {
    return worker_pool_.inline_mode();
}

void Scheduler::run_batch(const std::vector<std::function<void()>>& jobs) {
    worker_pool_.run_batch(jobs);
}

void Scheduler::run_async(std::function<void()> job) {
    worker_pool_.run_async(std::move(job));
}

void Scheduler::register_async_waiter(const AsyncWaitKey& key, const ScheduledTask& task) {
    async_completion_queue_.register_waiter(key, task);
    notify_activity();
}

void Scheduler::register_async_wait_group(const std::vector<AsyncWaitKey>& keys, const ScheduledTask& task) {
    async_completion_queue_.register_wait_group(keys, task);
    notify_activity();
}

void Scheduler::signal_async_completion(const AsyncWaitKey& key) {
    async_completion_queue_.signal_completion(key);
    notify_activity();
}

std::size_t Scheduler::promote_ready_async_tasks(uint64_t now_ns) {
    const std::size_t promoted = async_completion_queue_.promote_ready(work_queue_, now_ns);
    if (promoted != 0U) {
        notify_activity();
    }
    return promoted;
}

void Scheduler::remove_async_waiters_for_task(const ScheduledTask& task) {
    async_completion_queue_.remove_waiters_for_task(task);
    notify_activity();
}

bool Scheduler::has_async_waiters_for_run(RunId run_id) const {
    return async_completion_queue_.has_waiters_for_run(run_id);
}

bool Scheduler::has_async_waiters() const noexcept {
    return !async_completion_queue_.empty();
}

uint64_t Scheduler::activity_epoch() const {
    std::lock_guard<std::mutex> lock(activity_mutex_);
    return activity_epoch_;
}

void Scheduler::wait_for_activity(uint64_t observed_epoch, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(activity_mutex_);
    activity_cv_.wait_until(lock, deadline, [this, observed_epoch]() {
        return activity_epoch_ != observed_epoch;
    });
}

void Scheduler::wait_for_async_activity(std::chrono::milliseconds timeout) {
    wait_for_activity(activity_epoch(), std::chrono::steady_clock::now() + timeout);
}

void Scheduler::notify_activity() {
    {
        std::lock_guard<std::mutex> lock(activity_mutex_);
        activity_epoch_ += 1U;
    }
    activity_cv_.notify_all();
}

} // namespace agentcore
