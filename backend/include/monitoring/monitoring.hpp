#pragma once

#include "database/connection_pool.hpp"

#include <cstdint>

struct ConsistencyReport {
    std::int64_t duplicate_active_bookings = 0;
    std::int64_t duplicate_active_reservations = 0;
    std::int64_t seat_status_mismatches = 0;
    std::int64_t expired_active_reservations = 0;
    std::int64_t cancelled_seat_conflicts = 0;

    bool consistent() const;
};

ConsistencyReport check_database_consistency(ConnectionPool& pool);

