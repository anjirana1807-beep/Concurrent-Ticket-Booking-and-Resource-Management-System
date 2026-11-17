#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ScheduledRequest {
    std::string id;
    int priority;
    int arrival_seconds;
    int service_seconds = 1;
};

struct ScheduledOrder {
    std::vector<std::string> order;
    std::vector<int> waiting_seconds;
};

struct SchedulingComparison {
    ScheduledOrder fcfs;
    ScheduledOrder priority;
    ScheduledOrder aging;
};

SchedulingComparison compare_schedulers(const std::vector<ScheduledRequest>& requests,
                                         int aging_interval_seconds);
