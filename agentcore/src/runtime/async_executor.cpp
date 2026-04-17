#include "agentcore/runtime/async_executor.h"

#include <algorithm>

namespace agentcore {

namespace {

std::size_t detect_worker_count(std::size_t requested_workers) {
    if (requested_workers != 0U) {
        return std::max<std::size_t>(1U, requested_workers);
    }
    const std::size_t hardware_workers = std::thread::hardware_concurrency() == 0U
        ? 2U
        : static_cast<std::size_t>(std::thread::hardware_concurrency());
    return std::max<std::size_t>(2U, hardware_workers);
}

} // namespace

AsyncTaskExecutor::AsyncTaskExecutor(std::size_t worker_count, std::size_t max_queue_depth)
    : max_queue_depth_(max_queue_depth) {
    const std::size_t resolved_workers = detect_worker_count(worker_count);
    workers_.reserve(resolved_workers);
    for (std::size_t index = 0; index < resolved_workers; ++index) {
        workers_.emplace_back([this]() {
            worker_loop();
        });
    }
}

AsyncTaskExecutor::~AsyncTaskExecutor() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    job_cv_.notify_all();
    capacity_cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void AsyncTaskExecutor::enqueue(std::function<void()> job) {
    if (!job) {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (max_queue_depth_ != 0U) {
        capacity_cv_.wait(lock, [this]() {
            return stopping_ || jobs_.size() < max_queue_depth_;
        });
    }
    if (stopping_) {
        return;
    }

    jobs_.push_back(std::move(job));
    lock.unlock();
    job_cv_.notify_one();
}

std::size_t AsyncTaskExecutor::worker_count() const noexcept {
    return workers_.size();
}

std::size_t AsyncTaskExecutor::queue_size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

void AsyncTaskExecutor::worker_loop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            job_cv_.wait(lock, [this]() {
                return stopping_ || !jobs_.empty();
            });
            if (stopping_ && jobs_.empty()) {
                return;
            }

            job = std::move(jobs_.front());
            jobs_.pop_front();
            if (max_queue_depth_ != 0U) {
                capacity_cv_.notify_one();
            }
        }

        try {
            job();
        } catch (...) {
        }
    }
}

} // namespace agentcore
