#include "concurrency/deadlock_demo.hpp"

#include <array>
#include <barrier>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>

std::string run_deadlock_demonstration() {
    std::array<std::timed_mutex, 2> resources;
    std::barrier<> rendezvous(2);
    std::atomic<int> timed_out{0};
    auto unsafe_worker = [&](int first, int second) {
        std::unique_lock<std::timed_mutex> held(resources[static_cast<std::size_t>(first)]);
        rendezvous.arrive_and_wait();
        if (!resources[static_cast<std::size_t>(second)].try_lock_for(
                std::chrono::milliseconds(100))) {
            ++timed_out;
        } else {
            resources[static_cast<std::size_t>(second)].unlock();
        }
    };
    std::thread first(unsafe_worker, 0, 1);
    std::thread second(unsafe_worker, 1, 0);
    first.join();
    second.join();

    std::array<std::mutex, 2> ordered_resources;
    auto safe_worker = [&] {
        std::lock_guard low(ordered_resources[0]);
        std::lock_guard high(ordered_resources[1]);
    };
    std::thread safe_first(safe_worker);
    std::thread safe_second(safe_worker);
    safe_first.join();
    safe_second.join();
    return "{\"unsafe_order_timeouts\":" + std::to_string(timed_out.load()) +
           ",\"safe_order_completed\":true,"
           "\"explanation\":\"Opposite acquisition order creates a circular-wait risk; "
           "timed locking exposes it without hanging. The safe run always locks resource "
           "0 before resource 1. Contention is waiting for a lock; deadlock requires "
           "a cycle of waits.\"}";
}
