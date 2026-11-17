#include "booking/booking_service.hpp"
#include "database/query.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
bool retryable_booking_error(const pqxx::sql_error& error) {
    const std::string state = error.sqlstate();
    return state == "40P01" || state == "40001" || state == "55P03";
}
}

BookingService::BookingService(ConnectionPool& pool, std::chrono::seconds reservation_ttl)
    : pool_(pool), reservation_ttl_(reservation_ttl) {
    if (reservation_ttl_.count() <= 0) {
        throw std::invalid_argument("RESERVATION_TTL_SECONDS must be positive");
    }
}

std::unique_lock<std::mutex> BookingService::lock_seat(std::int64_t seat_id) {
    const auto index = std::hash<std::int64_t>{}(seat_id) % seat_mutexes_.size();
    return std::unique_lock<std::mutex>(seat_mutexes_[index]);
}

void BookingService::audit(pqxx::work& transaction, const std::string& event_type,
                           const std::string& details) {
    database::execute(transaction, 
        "INSERT INTO audit_log(event_type, details) VALUES ($1, $2::jsonb)",
        event_type, details);
}

void BookingService::audit_booking_failure(std::int64_t seat_id, std::int64_t user_id,
                                           const std::string& event_type,
                                           const std::string& reason) {
    auto lease = pool_.acquire();
    pqxx::work transaction(lease.connection());
    audit(transaction, event_type,
          "{\"seat_id\":" + std::to_string(seat_id) +
              ",\"user_id\":" + std::to_string(user_id) +
              ",\"reason\":\"" + reason + "\"}");
    transaction.commit();
}

std::int64_t BookingService::create_user(const std::string& name, const std::string& email) {
    auto lease = pool_.acquire();
    pqxx::work transaction(lease.connection());
    const auto row = database::execute_one(transaction, 
        "INSERT INTO users(name, email) VALUES ($1, $2) "
        "ON CONFLICT(email) DO UPDATE SET name = EXCLUDED.name RETURNING id",
        name, email);
    transaction.commit();
    return row[0].as<std::int64_t>();
}

BookingResult BookingService::reserve(std::int64_t seat_id, std::int64_t user_id) {
    auto seat_lock = lock_seat(seat_id);
    auto lease = pool_.acquire();
    pqxx::work transaction(lease.connection());
    const auto seat = database::execute(transaction, 
        "SELECT status FROM seats WHERE id = $1 FOR UPDATE", seat_id);
    if (seat.empty()) {
        audit(transaction, "reservation_rejected", "{\"reason\":\"seat_not_found\"}");
        transaction.commit();
        return {false, "Seat was not found.", 0, 0, "REJECTED"};
    }
    if (seat[0]["status"].as<std::string>() != "AVAILABLE") {
        audit(transaction, "reservation_rejected", "{\"reason\":\"seat_unavailable\"}");
        transaction.commit();
        return {false, "Sorry, this seat is already booked or reserved.", 0, 0, "REJECTED"};
    }
    const auto reservation = database::execute_one(transaction, 
        "INSERT INTO reservations(seat_id, user_id, status, expires_at) "
        "VALUES ($1, $2, 'ACTIVE', now() + ($3 * interval '1 second')) RETURNING id",
        seat_id, user_id, reservation_ttl_.count());
    database::execute(transaction, "UPDATE seats SET status = 'RESERVED' WHERE id = $1", seat_id);
    audit(transaction, "reservation_created",
          "{\"seat_id\":" + std::to_string(seat_id) + ",\"user_id\":" +
              std::to_string(user_id) + "}");
    transaction.commit();
    return {true, "Seat temporarily reserved. Complete payment before it expires.",
            0, reservation[0].as<std::int64_t>(), "RESERVED"};
}

BookingResult BookingService::pay(std::int64_t reservation_id, bool payment_succeeds) {
    auto lease = pool_.acquire();
    pqxx::work transaction(lease.connection());
    const auto reservation = database::execute(transaction, 
        "SELECT seat_id, user_id, status, expires_at <= now() AS expired "
        "FROM reservations WHERE id = $1 FOR UPDATE", reservation_id);
    if (reservation.empty() || reservation[0]["status"].as<std::string>() != "ACTIVE") {
        audit(transaction, "payment_failed", "{\"reason\":\"reservation_inactive\"}");
        transaction.commit();
        return {false, "Reservation is no longer active.", 0, reservation_id, "FAILED"};
    }
    const auto seat_id = reservation[0]["seat_id"].as<std::int64_t>();
    const auto user_id = reservation[0]["user_id"].as<std::int64_t>();
    database::execute(transaction, "SELECT id FROM seats WHERE id = $1 FOR UPDATE", seat_id);
    if (reservation[0]["expired"].as<bool>()) {
        database::execute(transaction, 
            "UPDATE reservations SET status = 'EXPIRED' WHERE id = $1", reservation_id);
        database::execute(transaction, 
            "UPDATE seats SET status = 'AVAILABLE' WHERE id = $1 AND status = 'RESERVED'",
            seat_id);
        audit(transaction, "reservation_expired",
              "{\"reservation_id\":" + std::to_string(reservation_id) + "}");
        const auto offered = database::execute(transaction, 
            "UPDATE waiting_list SET status='PROCESSED' "
            "WHERE seat_id=$1 AND user_id=$2 AND status='OFFERED' RETURNING id",
            seat_id, user_id);
        if (!offered.empty()) {
            audit(transaction, "waiting_list_processed",
                  "{\"waiting_id\":" + std::to_string(offered[0]["id"].as<std::int64_t>()) +
                      ",\"reason\":\"offer_expired\"}");
        }
        offer_next_waiter(transaction, seat_id);
        transaction.commit();
        return {false, "Reservation expired. Please select the seat again.",
                0, reservation_id, "EXPIRED"};
    }
    const auto booking = database::execute_one(transaction, 
        "INSERT INTO bookings(seat_id, user_id, reservation_id, status) "
        "VALUES ($1, $2, $3, 'PENDING') RETURNING id",
        seat_id, user_id, reservation_id);
    const auto booking_id = booking[0].as<std::int64_t>();
    if (payment_succeeds) {
        database::execute(transaction, 
            "INSERT INTO payments(booking_id, status, amount_cents) VALUES ($1, 'SUCCEEDED', 0)",
            booking_id);
        database::execute(transaction, 
            "UPDATE bookings SET status = 'CONFIRMED', updated_at = now() WHERE id = $1",
            booking_id);
        database::execute(transaction, "UPDATE reservations SET status = 'CONFIRMED' WHERE id = $1",
                                reservation_id);
        database::execute(transaction, "UPDATE seats SET status = 'BOOKED' WHERE id = $1", seat_id);
        const auto offered = database::execute(transaction, 
            "UPDATE waiting_list SET status='PROCESSED' "
            "WHERE seat_id=$1 AND user_id=$2 AND status='OFFERED' RETURNING id",
            seat_id, user_id);
        if (!offered.empty()) {
            audit(transaction, "waiting_list_processed",
                  "{\"waiting_id\":" + std::to_string(offered[0]["id"].as<std::int64_t>()) +
                      ",\"reason\":\"offer_accepted\"}");
        }
        audit(transaction, "booking_succeeded",
              "{\"booking_id\":" + std::to_string(booking_id) + "}");
        transaction.commit();
        return {true, "Booking confirmed.", booking_id, reservation_id, "CONFIRMED"};
    }
    database::execute(transaction, 
        "INSERT INTO payments(booking_id, status, amount_cents) VALUES ($1, 'FAILED', 0)",
        booking_id);
    database::execute(transaction, 
        "UPDATE bookings SET status = 'FAILED', updated_at = now() WHERE id = $1", booking_id);
    database::execute(transaction, "UPDATE reservations SET status = 'RELEASED' WHERE id = $1",
                            reservation_id);
    database::execute(transaction, "UPDATE seats SET status = 'AVAILABLE' WHERE id = $1", seat_id);
    const auto offered = database::execute(transaction, 
        "UPDATE waiting_list SET status='PROCESSED' "
        "WHERE seat_id=$1 AND user_id=$2 AND status='OFFERED' RETURNING id",
        seat_id, user_id);
    if (!offered.empty()) {
        audit(transaction, "waiting_list_processed",
              "{\"waiting_id\":" + std::to_string(offered[0]["id"].as<std::int64_t>()) +
                  ",\"reason\":\"payment_failed\"}");
    }
    audit(transaction, "payment_failed",
          "{\"booking_id\":" + std::to_string(booking_id) + "}");
    offer_next_waiter(transaction, seat_id);
    transaction.commit();
    return {false, "Simulated payment failed; reservation released.", booking_id,
            reservation_id, "FAILED"};
}

BookingResult BookingService::book(std::int64_t seat_id, std::int64_t user_id,
                                   bool payment_succeeds) {
    auto seat_lock = lock_seat(seat_id);
    constexpr std::size_t max_attempts = 3;
    const auto record_failure = [&](const char* event_type, const char* reason) {
        try {
            audit_booking_failure(seat_id, user_id, event_type, reason);
        } catch (const std::exception& audit_error) {
            std::cerr << "Booking failure audit could not be committed: "
                      << audit_error.what() << '\n';
        }
    };
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        try {
            auto lease = pool_.acquire();
            pqxx::work transaction(lease.connection());
            transaction.exec("SET LOCAL lock_timeout = '2s'");
            const auto seat = database::execute(
                transaction, "SELECT status FROM seats WHERE id = $1 FOR UPDATE", seat_id);
            if (seat.empty()) {
                audit(transaction, "booking_failed", "{\"reason\":\"seat_not_found\"}");
                transaction.commit();
                return {false, "Seat was not found.", 0, 0, "REJECTED"};
            }
            if (seat[0]["status"].as<std::string>() != "AVAILABLE") {
                audit(transaction, "booking_failed",
                      "{\"seat_id\":" + std::to_string(seat_id) +
                          ",\"reason\":\"seat_unavailable\"}");
                transaction.commit();
                return {false, "Sorry, this seat is already booked.", 0, 0, "REJECTED"};
            }
            const auto reservation = database::execute_one(
                transaction,
                "INSERT INTO reservations(seat_id, user_id, status, expires_at) "
                "VALUES ($1, $2, 'ACTIVE', now() + ($3 * interval '1 second')) RETURNING id",
                seat_id, user_id, reservation_ttl_.count());
            const auto reservation_id = reservation[0].as<std::int64_t>();
            database::execute(transaction,
                              "UPDATE seats SET status = 'RESERVED' WHERE id = $1", seat_id);
            const auto booking = database::execute_one(
                transaction,
                "INSERT INTO bookings(seat_id, user_id, reservation_id, status) "
                "VALUES ($1, $2, $3, 'PENDING') RETURNING id",
                seat_id, user_id, reservation_id);
            const auto booking_id = booking[0].as<std::int64_t>();
            database::execute(
                transaction,
                "INSERT INTO payments(booking_id, status, amount_cents) VALUES ($1, $2, 0)",
                booking_id, payment_succeeds ? "SUCCEEDED" : "FAILED");
            if (payment_succeeds) {
                database::execute(
                    transaction,
                    "UPDATE bookings SET status = 'CONFIRMED', updated_at = now() WHERE id = $1",
                    booking_id);
                database::execute(transaction,
                                  "UPDATE reservations SET status = 'CONFIRMED' WHERE id = $1",
                                  reservation_id);
                database::execute(transaction,
                                  "UPDATE seats SET status = 'BOOKED' WHERE id = $1", seat_id);
                audit(transaction, "booking_succeeded",
                      "{\"booking_id\":" + std::to_string(booking_id) + "}");
                transaction.commit();
                return {true, "Booking confirmed.", booking_id, reservation_id, "CONFIRMED"};
            }
            database::execute(
                transaction,
                "UPDATE bookings SET status = 'FAILED', updated_at = now() WHERE id = $1",
                booking_id);
            database::execute(transaction,
                              "UPDATE reservations SET status = 'RELEASED' WHERE id = $1",
                              reservation_id);
            database::execute(transaction,
                              "UPDATE seats SET status = 'AVAILABLE' WHERE id = $1", seat_id);
            audit(transaction, "payment_failed",
                  "{\"booking_id\":" + std::to_string(booking_id) + "}");
            offer_next_waiter(transaction, seat_id);
            transaction.commit();
            return {false, "Simulated payment failed; reservation released.", booking_id,
                    reservation_id, "FAILED"};
        } catch (const pqxx::sql_error& error) {
            if (retryable_booking_error(error) && attempt + 1 < max_attempts) {
                record_failure("booking_retry", "retryable_database_error");
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50 * static_cast<int>(attempt + 1)));
                continue;
            }
            record_failure("booking_failed", "database_error");
            throw;
        } catch (const std::exception&) {
            record_failure("booking_failed", "booking_error");
            throw;
        }
    }
    throw std::logic_error("Booking retry loop ended unexpectedly");
}

BookingResult BookingService::cancel(std::int64_t booking_id, std::int64_t user_id) {
    auto lease = pool_.acquire();
    pqxx::work transaction(lease.connection());
    const auto booking = database::execute(transaction, 
        "SELECT seat_id, status FROM bookings WHERE id = $1 AND user_id = $2 FOR UPDATE",
        booking_id, user_id);
    if (booking.empty() || booking[0]["status"].as<std::string>() != "CONFIRMED") {
        audit(transaction, "cancellation_rejected",
              "{\"booking_id\":" + std::to_string(booking_id) + "}");
        transaction.commit();
        return {false, "Confirmed booking was not found for this user.", booking_id, 0, "REJECTED"};
    }
    const auto seat_id = booking[0]["seat_id"].as<std::int64_t>();
    database::execute(transaction, "SELECT id FROM seats WHERE id = $1 FOR UPDATE", seat_id);
    database::execute(transaction, 
        "UPDATE bookings SET status = 'CANCELLED', updated_at = now() WHERE id = $1", booking_id);
    database::execute(transaction, "UPDATE seats SET status = 'AVAILABLE' WHERE id = $1", seat_id);
    audit(transaction, "booking_cancelled",
          "{\"booking_id\":" + std::to_string(booking_id) + "}");
    offer_next_waiter(transaction, seat_id);
    transaction.commit();
    return {true, "Booking cancelled and seat released.", booking_id, 0, "CANCELLED"};
}

BookingResult BookingService::join_waiting_list(std::int64_t seat_id, std::int64_t user_id) {
    auto lease = pool_.acquire();
    pqxx::work transaction(lease.connection());
    const auto seat = database::execute(transaction, 
        "SELECT status FROM seats WHERE id = $1 FOR UPDATE", seat_id);
    if (seat.empty()) {
        transaction.commit();
        return {false, "Seat was not found.", 0, 0, "REJECTED"};
    }
    if (seat[0]["status"].as<std::string>() == "AVAILABLE") {
        transaction.commit();
        return {false, "Seat is available; reserve it directly.", 0, 0, "AVAILABLE"};
    }
    database::execute(transaction, 
        "INSERT INTO waiting_list(seat_id, user_id, status) VALUES ($1, $2, 'WAITING') "
        "ON CONFLICT DO NOTHING", seat_id, user_id);
    audit(transaction, "waiting_list_joined",
          "{\"seat_id\":" + std::to_string(seat_id) + ",\"user_id\":" +
              std::to_string(user_id) + "}");
    transaction.commit();
    return {true, "Added to the waiting list.", 0, 0, "WAITING"};
}

std::int64_t BookingService::offer_next_waiter(pqxx::work& transaction, std::int64_t seat_id) {
    const auto next = database::execute(transaction, 
        "SELECT id, user_id FROM waiting_list WHERE seat_id = $1 AND status = 'WAITING' "
        "ORDER BY created_at, id LIMIT 1 FOR UPDATE SKIP LOCKED", seat_id);
    if (next.empty()) return 0;
    const auto wait_id = next[0]["id"].as<std::int64_t>();
    const auto user_id = next[0]["user_id"].as<std::int64_t>();
    const auto reservation = database::execute_one(transaction, 
        "INSERT INTO reservations(seat_id, user_id, status, expires_at) "
        "VALUES ($1, $2, 'ACTIVE', now() + ($3 * interval '1 second')) RETURNING id",
        seat_id, user_id, reservation_ttl_.count());
    database::execute(transaction, "UPDATE seats SET status = 'RESERVED' WHERE id = $1", seat_id);
    database::execute(transaction, 
        "UPDATE waiting_list SET status = 'OFFERED' WHERE id = $1", wait_id);
    audit(transaction, "waiting_list_offered",
          "{\"waiting_id\":" + std::to_string(wait_id) + ",\"reservation_id\":" +
              std::to_string(reservation[0].as<std::int64_t>()) + "}");
    return reservation[0].as<std::int64_t>();
}

std::size_t BookingService::expire_reservations() {
    auto lease = pool_.acquire();
    pqxx::work transaction(lease.connection());
    const auto expired = transaction.exec(
        "SELECT id, seat_id, user_id FROM reservations "
        "WHERE status = 'ACTIVE' AND expires_at <= now() "
        "ORDER BY expires_at FOR UPDATE SKIP LOCKED");
    std::size_t count = 0;
    for (const auto& row : expired) {
        const auto reservation_id = row["id"].as<std::int64_t>();
        const auto seat_id = row["seat_id"].as<std::int64_t>();
        const auto user_id = row["user_id"].as<std::int64_t>();
        database::execute(transaction, "SELECT id FROM seats WHERE id = $1 FOR UPDATE", seat_id);
        database::execute(transaction, "UPDATE reservations SET status = 'EXPIRED' WHERE id = $1",
                                reservation_id);
        database::execute(transaction, 
            "UPDATE seats SET status = 'AVAILABLE' WHERE id = $1 AND status = 'RESERVED'",
            seat_id);
        audit(transaction, "reservation_expired",
              "{\"reservation_id\":" + std::to_string(reservation_id) + "}");
        const auto offered = database::execute(transaction, 
            "UPDATE waiting_list SET status='PROCESSED' "
            "WHERE seat_id=$1 AND user_id=$2 AND status='OFFERED' RETURNING id",
            seat_id, user_id);
        if (!offered.empty()) {
            audit(transaction, "waiting_list_processed",
                  "{\"waiting_id\":" + std::to_string(offered[0]["id"].as<std::int64_t>()) +
                      ",\"reason\":\"offer_expired\"}");
        }
        offer_next_waiter(transaction, seat_id);
        ++count;
    }
    transaction.commit();
    return count;
}
