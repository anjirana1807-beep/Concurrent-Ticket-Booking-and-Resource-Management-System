#include "monitoring/monitoring.hpp"
#include "database/query.hpp"

#include <pqxx/pqxx>

bool ConsistencyReport::consistent() const {
    return duplicate_active_bookings == 0 &&
           duplicate_active_reservations == 0 &&
           seat_status_mismatches == 0 &&
           expired_active_reservations == 0 &&
           cancelled_seat_conflicts == 0;
}

ConsistencyReport check_database_consistency(ConnectionPool& pool) {
    auto lease = pool.acquire();
    pqxx::read_transaction transaction(lease.connection());
    const auto row = transaction.exec(
        "SELECT "
        "(SELECT count(*) FROM (SELECT seat_id FROM bookings "
        "WHERE status IN ('PENDING','CONFIRMED') GROUP BY seat_id HAVING count(*) > 1) d) "
        "AS duplicate_active_bookings, "
        "(SELECT count(*) FROM (SELECT seat_id FROM reservations "
        "WHERE status='ACTIVE' GROUP BY seat_id HAVING count(*) > 1) r) "
        "AS duplicate_active_reservations, "
        "(SELECT count(*) FROM seats s WHERE "
        "(s.status='BOOKED' AND NOT EXISTS (SELECT 1 FROM bookings b "
        "WHERE b.seat_id=s.id AND b.status='CONFIRMED')) OR "
        "(s.status='RESERVED' AND NOT EXISTS (SELECT 1 FROM reservations r "
        "WHERE r.seat_id=s.id AND r.status='ACTIVE' AND r.expires_at > now())) OR "
        "(s.status='AVAILABLE' AND EXISTS (SELECT 1 FROM bookings b "
        "WHERE b.seat_id=s.id AND b.status IN ('PENDING','CONFIRMED')))) "
        "AS seat_status_mismatches, "
        "(SELECT count(*) FROM reservations WHERE status='ACTIVE' AND expires_at <= now()) "
        "AS expired_active_reservations, "
        "(SELECT count(*) FROM bookings b JOIN seats s ON s.id=b.seat_id "
        "WHERE b.status='CANCELLED' AND s.status='BOOKED' "
        "AND NOT EXISTS (SELECT 1 FROM bookings active "
        "WHERE active.seat_id=s.id AND active.status='CONFIRMED')) "
        "AS cancelled_seat_conflicts")
                           .one_row();
    return {
        row["duplicate_active_bookings"].as<std::int64_t>(),
        row["duplicate_active_reservations"].as<std::int64_t>(),
        row["seat_status_mismatches"].as<std::int64_t>(),
        row["expired_active_reservations"].as<std::int64_t>(),
        row["cancelled_seat_conflicts"].as<std::int64_t>()
    };
}
