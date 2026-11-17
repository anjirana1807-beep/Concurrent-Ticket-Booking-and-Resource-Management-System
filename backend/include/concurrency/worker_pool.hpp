#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

enum class SchedulingPolicy { Fcfs, Priority, Aging };

SchedulingPolicy parse_policy(const std::string& value);
std::string policy_name(SchedulingPolicy policy);

class WorkerPool {
public:
    WorkerPool(std::size_t worker_count, std::size_t queue_capacity,
               SchedulingPolicy policy, std::chrono::seconds aging_interval);
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    ~WorkerPool();

    template <typename Function>
    auto submit(int priority, Function&& function) -> std::future<std::invoke_result_t<Function>> {
        using Result = std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto result = task->get_future();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                throw std::runtime_error("Worker pool is stopping");
            }
            if (queue_.size() >= queue_capacity_) {
                throw std::runtime_error("Request queue is full; retry shortly");
            }
            queue_.push_back(QueuedTask{priority, std::chrono::steady_clock::now(),
                                        next_sequence_++,
                                        [task] { (*task)(); }});
            if (queue_.size() > peak_queue_size_) peak_queue_size_ = queue_.size();
        }
        condition_.notify_one();
        return result;
    }

    std::size_t worker_count() const;
    std::size_t active_workers() const;
    std::size_t queue_size() const;
    std::size_t peak_queue_size() const;
    SchedulingPolicy policy() const;

private:
    struct QueuedTask {
        int priority;
        std::chrono::steady_clock::time_point enqueued_at;
        std::size_t sequence;
        std::function<void()> run;
    };

    void worker_loop();
    std::size_t next_task_index() const;

    std::size_t queue_capacity_;
    SchedulingPolicy policy_;
    std::chrono::seconds aging_interval_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedTask> queue_;
    std::vector<std::thread> workers_;
    std::size_t active_workers_ = 0;
    std::size_t peak_queue_size_ = 0;
    std::size_t next_sequence_ = 0;
    bool stopping_ = false;
};
