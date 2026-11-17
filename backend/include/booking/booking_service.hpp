#pragma once

#include "database/connection_pool.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

struct BookingResult {
    bool success = false;
    std::string message;
    std::int64_t booking_id = 0;
    std::int64_t reservation_id = 0;
    std::string status;
};

class BookingService {
public:
    BookingService(ConnectionPool& pool, std::chrono::seconds reservation_ttl);

    std::int64_t create_user(const std::string& name, const std::string& email);
    BookingResult reserve(std::int64_t seat_id, std::int64_t user_id);
    BookingResult pay(std::int64_t reservation_id, bool payment_succeeds);
    BookingResult book(std::int64_t seat_id, std::int64_t user_id,
                       bool payment_succeeds);
    BookingResult cancel(std::int64_t booking_id, std::int64_t user_id);
    BookingResult join_waiting_list(std::int64_t seat_id, std::int64_t user_id);
    std::size_t expire_reservations();

private:
    std::int64_t offer_next_waiter(pqxx::work& transaction, std::int64_t seat_id);
    std::unique_lock<std::mutex> lock_seat(std::int64_t seat_id);
    void audit(pqxx::work& transaction, const std::string& event_type,
               const std::string& details);
    void audit_booking_failure(std::int64_t seat_id, std::int64_t user_id,
                               const std::string& event_type, const std::string& reason);

    ConnectionPool& pool_;
    std::chrono::seconds reservation_ttl_;
    std::array<std::mutex, 64> seat_mutexes_;
};
