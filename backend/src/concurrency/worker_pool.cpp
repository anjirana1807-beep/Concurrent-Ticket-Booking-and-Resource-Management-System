#include "concurrency/worker_pool.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

SchedulingPolicy parse_policy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (normalized == "FCFS") return SchedulingPolicy::Fcfs;
    if (normalized == "PRIORITY") return SchedulingPolicy::Priority;
    if (normalized == "AGING") return SchedulingPolicy::Aging;
    throw std::invalid_argument("SCHEDULING_POLICY must be FCFS, PRIORITY, or AGING");
}

std::string policy_name(SchedulingPolicy policy) {
    switch (policy) {
    case SchedulingPolicy::Fcfs: return "FCFS";
    case SchedulingPolicy::Priority: return "PRIORITY";
    case SchedulingPolicy::Aging: return "AGING";
    }
    throw std::logic_error("Unknown scheduling policy");
}

WorkerPool::WorkerPool(std::size_t worker_count, std::size_t queue_capacity,
                       SchedulingPolicy policy, std::chrono::seconds aging_interval)
    : queue_capacity_(queue_capacity), policy_(policy), aging_interval_(aging_interval) {
    if (worker_count == 0 || queue_capacity == 0 || aging_interval_.count() <= 0) {
        throw std::invalid_argument("Worker count, queue capacity, and aging interval must be positive");
    }
    workers_.reserve(worker_count);
    try {
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        throw;
    }
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

std::size_t WorkerPool::worker_count() const {
    return workers_.size();
}

std::size_t WorkerPool::active_workers() const {
    std::lock_guard lock(mutex_);
    return active_workers_;
}

std::size_t WorkerPool::queue_size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

std::size_t WorkerPool::peak_queue_size() const {
    std::lock_guard lock(mutex_);
    return peak_queue_size_;
}

SchedulingPolicy WorkerPool::policy() const {
    return policy_;
}

std::size_t WorkerPool::next_task_index() const {
    if (policy_ == SchedulingPolicy::Fcfs) return 0;

    const auto now = std::chrono::steady_clock::now();
    std::size_t selected = 0;
    long long best_score = std::numeric_limits<long long>::min();
    for (std::size_t i = 0; i < queue_.size(); ++i) {
        long long score = queue_[i].priority;
        if (policy_ == SchedulingPolicy::Aging) {
            const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
                now - queue_[i].enqueued_at).count();
            score += waited / aging_interval_.count();
        }
        if (score > best_score ||
            (score == best_score && queue_[i].sequence < queue_[selected].sequence)) {
            best_score = score;
            selected = i;
        }
    }
    return selected;
}

void WorkerPool::worker_loop() {
    for (;;) {
        QueuedTask task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            const auto index = next_task_index();
            task = std::move(queue_[index]);
            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(index));
            ++active_workers_;
        }
        task.run();
        {
            std::lock_guard lock(mutex_);
            --active_workers_;
        }
    }
}
