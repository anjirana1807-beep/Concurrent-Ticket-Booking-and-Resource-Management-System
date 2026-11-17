#include "scheduler/scheduler.hpp"

#include <algorithm>
#include <stdexcept>

namespace {
ScheduledOrder simulate(const std::vector<ScheduledRequest>& requests,
                        int aging_interval_seconds, bool priority, bool aging) {
    if (aging_interval_seconds <= 0) {
        throw std::invalid_argument("Aging interval must be positive");
    }
    std::vector<bool> complete(requests.size(), false);
    ScheduledOrder result;
    int time = 0;
    std::size_t done = 0;
    while (done < requests.size()) {
        std::size_t selected = requests.size();
        long long best = -1;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            if (complete[i] || requests[i].arrival_seconds > time) continue;
            long long score = 0;
            if (priority) {
                score = requests[i].priority;
                if (aging) {
                    score += (time - requests[i].arrival_seconds) /
                             aging_interval_seconds;
                }
            }
            if (selected == requests.size() || score > best ||
                (score == best && requests[i].arrival_seconds <
                                      requests[selected].arrival_seconds)) {
                selected = i;
                best = score;
            }
        }
        if (selected == requests.size()) {
            int next_arrival = -1;
            for (std::size_t i = 0; i < requests.size(); ++i) {
                if (!complete[i] &&
                    (next_arrival < 0 || requests[i].arrival_seconds < next_arrival)) {
                    next_arrival = requests[i].arrival_seconds;
                }
            }
            time = next_arrival;
            continue;
        }
        complete[selected] = true;
        result.order.push_back(requests[selected].id);
        result.waiting_seconds.push_back(time - requests[selected].arrival_seconds);
        time += requests[selected].service_seconds;
        ++done;
    }
    return result;
}
}

SchedulingComparison compare_schedulers(const std::vector<ScheduledRequest>& requests,
                                         int aging_interval_seconds) {
    for (const auto& request : requests) {
        if (request.arrival_seconds < 0 || request.service_seconds <= 0) {
            throw std::invalid_argument("Arrival must be non-negative and service time positive");
        }
    }
    return {simulate(requests, aging_interval_seconds, false, false),
            simulate(requests, aging_interval_seconds, true, false),
            simulate(requests, aging_interval_seconds, true, true)};
}
