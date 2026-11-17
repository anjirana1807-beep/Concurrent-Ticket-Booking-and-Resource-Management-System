#include "booking/booking_service.hpp"
#include "concurrency/deadlock_demo.hpp"
#include "concurrency/worker_pool.hpp"
#include "database/connection_pool.hpp"
#include "database/query.hpp"
#include "monitoring/monitoring.hpp"
#include "reservation/reservation_worker.hpp"
#include "scheduler/scheduler.hpp"

#include <crow.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string env_string(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

bool env_bool(const char* name, bool fallback) {
    auto value = env_string(name, "");
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value.empty()) return fallback;
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    throw std::invalid_argument(std::string(name) + " must be true or false");
}

bool simulated_payment_success(double failure_rate) {
    static std::mutex random_mutex;
    static std::mt19937 generator(std::random_device{}());
    std::lock_guard lock(random_mutex);
    return std::bernoulli_distribution(1.0 - failure_rate)(generator);
}

std::size_t env_size(const char* name, std::size_t fallback) {
    const auto value = env_string(name, "");
    if (value.empty()) return fallback;
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0) {
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    }
    return static_cast<std::size_t>(parsed);
}

crow::response json_response(int status, crow::json::wvalue value) {
    crow::response response(status);
    response.set_header("Content-Type", "application/json; charset=utf-8");
    response.body = value.dump();
    return response;
}

crow::response message_response(int status, const std::string& message) {
    crow::json::wvalue body;
    body["message"] = message;
    body["success"] = status < 400;
    return json_response(status, std::move(body));
}

crow::response booking_response(const BookingResult& result) {
    crow::json::wvalue body;
    body["success"] = result.success;
    body["message"] = result.message;
    body["status"] = result.status;
    body["booking_id"] = result.booking_id;
    body["reservation_id"] = result.reservation_id;
    return json_response(result.success ? 200 : 409, std::move(body));
}

crow::response static_file(const std::string& path, const std::string& content_type) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return message_response(404, "Frontend file not found: " + path);
    const std::string body((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    crow::response response(body);
    response.set_header("Content-Type", content_type);
    return response;
}

void log_exception(const char* context, const std::exception& error) {
    std::cerr << context << ": " << error.what() << '\n';
}

crow::json::wvalue schedule_json(const ScheduledOrder& order) {
    crow::json::wvalue result;
    crow::json::wvalue::list ids;
    crow::json::wvalue::list waiting;
    for (const auto& id : order.order) ids.emplace_back(id);
    for (const auto seconds : order.waiting_seconds) waiting.emplace_back(seconds);
    result["order"] = std::move(ids);
    result["waiting_seconds"] = std::move(waiting);
    return result;
}
}

int main() {
    try {
        const auto demo_mode = env_bool("DEMO_MODE", false);
        const auto connection_string = env_string("DATABASE_URL", "");
        if (connection_string.empty()) {
            throw std::invalid_argument("DATABASE_URL must be set in the server environment");
        }
        const auto connection_limit = env_size("DB_CONNECTION_LIMIT", 8);
        const auto worker_count = env_size("WORKER_COUNT", 4);
        const auto queue_capacity = env_size("REQUEST_QUEUE_CAPACITY", 128);
        const auto ttl = std::chrono::seconds(env_size("RESERVATION_TTL_SECONDS", 120));
        const auto aging_interval = std::chrono::seconds(env_size("AGING_INTERVAL_SECONDS", 5));
        const auto policy = parse_policy(env_string("SCHEDULING_POLICY", "FCFS"));
        const std::string address = demo_mode ? "127.0.0.1"
                                              : env_string("SERVER_ADDRESS", "0.0.0.0");
        const auto configured_port = env_size("SERVER_PORT", 18080);
        if (configured_port > 65535) {
            throw std::invalid_argument("SERVER_PORT must be between 1 and 65535");
        }
        const auto port = static_cast<std::uint16_t>(configured_port);
        const auto failure_rate = std::stod(env_string("PAYMENT_FAILURE_RATE", "0"));
        if (failure_rate < 0.0 || failure_rate > 1.0) {
            throw std::invalid_argument("PAYMENT_FAILURE_RATE must be between 0 and 1");
        }

        ConnectionPool pool(connection_string, connection_limit);
        BookingService bookings(pool, ttl);
        WorkerPool workers(worker_count, queue_capacity, policy, aging_interval);
        ReservationExpiryWorker expiry_worker(bookings);

        crow::SimpleApp app;
        CROW_ROUTE(app, "/")([] {
            return static_file("frontend/index.html", "text/html; charset=utf-8");
        });
        CROW_ROUTE(app, "/css/<string>")([](const std::string& filename) {
            if (filename != "styles.css") return message_response(404, "Not found");
            return static_file("frontend/css/styles.css", "text/css; charset=utf-8");
        });
        CROW_ROUTE(app, "/js/<string>")([](const std::string& filename) {
            if (filename != "app.js") return message_response(404, "Not found");
            return static_file("frontend/js/app.js", "text/javascript; charset=utf-8");
        });

        CROW_ROUTE(app, "/api/health")([&pool, &workers, demo_mode] {
            crow::json::wvalue result;
            result["status"] = "ok";
            result["demo_mode"] = demo_mode;
            if (!demo_mode) {
                result["database_connections_in_use"] = pool.in_use();
                result["active_workers"] = workers.active_workers();
            }
            return json_response(200, std::move(result));
        });

        CROW_ROUTE(app, "/api/users/<int>/bookings")([&pool](int user_id) {
            auto lease = pool.acquire();
            pqxx::read_transaction transaction(lease.connection());
            const auto rows = database::execute(transaction, 
                "SELECT b.id, b.status, b.created_at, e.name AS event_name, s.seat_label "
                "FROM bookings b JOIN seats s ON s.id=b.seat_id "
                "JOIN events e ON e.id=s.event_id WHERE b.user_id=$1 "
                "ORDER BY b.created_at DESC", user_id);
            crow::json::wvalue::list bookings_list;
            for (const auto& row : rows) {
                crow::json::wvalue booking;
                booking["id"] = row["id"].as<std::int64_t>();
                booking["status"] = row["status"].as<std::string>();
                booking["created_at"] = row["created_at"].as<std::string>();
                booking["event_name"] = row["event_name"].as<std::string>();
                booking["seat_label"] = row["seat_label"].as<std::string>();
                bookings_list.emplace_back(std::move(booking));
            }
            crow::json::wvalue result;
            result["bookings"] = std::move(bookings_list);
            return json_response(200, std::move(result));
        });

        CROW_ROUTE(app, "/api/users/<int>/reservations")([&pool](int user_id) {
            auto lease = pool.acquire();
            pqxx::read_transaction transaction(lease.connection());
            const auto rows = database::execute(transaction, 
                "SELECT r.id, r.seat_id, s.seat_label, r.expires_at "
                "FROM reservations r JOIN seats s ON s.id=r.seat_id "
                "WHERE r.user_id=$1 AND r.status='ACTIVE' AND r.expires_at > now() "
                "ORDER BY r.created_at DESC", user_id);
            crow::json::wvalue::list reservations;
            for (const auto& row : rows) {
                crow::json::wvalue item;
                item["id"] = row["id"].as<std::int64_t>();
                item["seat_id"] = row["seat_id"].as<std::int64_t>();
                item["seat_label"] = row["seat_label"].as<std::string>();
                item["expires_at"] = row["expires_at"].as<std::string>();
                reservations.emplace_back(std::move(item));
            }
            crow::json::wvalue result;
            result["reservations"] = std::move(reservations);
            return json_response(200, std::move(result));
        });

        CROW_ROUTE(app, "/api/events")([&pool] {
            auto lease = pool.acquire();
            pqxx::read_transaction transaction(lease.connection());
            const auto rows = transaction.exec(
                "SELECT id, name, description, venue, starts_at FROM events ORDER BY starts_at");
            crow::json::wvalue::list events;
            for (const auto& row : rows) {
                crow::json::wvalue event;
                event["id"] = row["id"].as<std::int64_t>();
                event["name"] = row["name"].as<std::string>();
                event["description"] = row["description"].as<std::string>();
                event["venue"] = row["venue"].as<std::string>();
                event["starts_at"] = row["starts_at"].as<std::string>();
                events.emplace_back(std::move(event));
            }
            crow::json::wvalue result;
            result["events"] = std::move(events);
            return json_response(200, std::move(result));
        });

        CROW_ROUTE(app, "/api/events/<int>/seats")([&pool](int event_id) {
            auto lease = pool.acquire();
            pqxx::read_transaction transaction(lease.connection());
            const auto rows = database::execute(transaction, 
                "SELECT id, seat_label, status FROM seats WHERE event_id = $1 ORDER BY seat_label",
                event_id);
            crow::json::wvalue::list seats;
            for (const auto& row : rows) {
                crow::json::wvalue seat;
                seat["id"] = row["id"].as<std::int64_t>();
                seat["label"] = row["seat_label"].as<std::string>();
                seat["status"] = row["status"].as<std::string>();
                seats.emplace_back(std::move(seat));
            }
            crow::json::wvalue result;
            result["seats"] = std::move(seats);
            return json_response(200, std::move(result));
        });

        CROW_ROUTE(app, "/api/users").methods(crow::HTTPMethod::POST)(
            [&bookings](const crow::request& request) {
                const auto body = crow::json::load(request.body);
                if (!body || !body.has("name") || !body.has("email")) {
                    return message_response(400, "JSON name and email are required.");
                }
                try {
                    const auto id = bookings.create_user(body["name"].s(), body["email"].s());
                    crow::json::wvalue result;
                    result["success"] = true;
                    result["user_id"] = id;
                    return json_response(200, std::move(result));
                } catch (const std::exception& error) {
                    log_exception("Create user", error);
                    return message_response(400, "Could not create user; check the email and name.");
                }
            });

        CROW_ROUTE(app, "/api/bookings").methods(crow::HTTPMethod::POST)(
            [&bookings, &workers, failure_rate](const crow::request& request) {
                const auto body = crow::json::load(request.body);
                if (!body || !body.has("seat_id") || !body.has("user_id")) {
                    return message_response(400, "JSON seat_id and user_id are required.");
                }
                const auto seat_id = body["seat_id"].i();
                const auto user_id = body["user_id"].i();
                const auto priority = body.has("priority") ? body["priority"].i() : 0;
                const auto succeeds = body.has("payment_success")
                    ? body["payment_success"].b()
                    : simulated_payment_success(failure_rate);
                try {
                    auto result = workers.submit(priority, [&bookings, seat_id, user_id, succeeds] {
                        return bookings.book(seat_id, user_id, succeeds);
                    }).get();
                    return booking_response(result);
                } catch (const std::exception& error) {
                    log_exception("Booking request", error);
                    const std::string message = error.what();
                    const bool queue_full =
                        message.find("queue is full") != std::string::npos;
                    return message_response(
                        queue_full ? 503 : 500,
                        queue_full ? "Booking queue is full; retry shortly."
                                   : "Could not process booking request.");
                }
            });

        CROW_ROUTE(app, "/api/reservations").methods(crow::HTTPMethod::POST)(
            [&bookings](const crow::request& request) {
                const auto body = crow::json::load(request.body);
                if (!body || !body.has("seat_id") || !body.has("user_id")) {
                    return message_response(400, "JSON seat_id and user_id are required.");
                }
                try {
                    return booking_response(
                        bookings.reserve(body["seat_id"].i(), body["user_id"].i()));
                } catch (const std::exception& error) {
                    log_exception("Create reservation", error);
                    return message_response(500, "Could not create reservation.");
                }
            });

        CROW_ROUTE(app, "/api/reservations/<int>/pay").methods(crow::HTTPMethod::POST)(
            [&bookings](const crow::request& request, int reservation_id) {
                const auto body = crow::json::load(request.body);
                if (!body || !body.has("payment_success")) {
                    return message_response(400, "JSON payment_success is required.");
                }
                try {
                    return booking_response(bookings.pay(reservation_id,
                                                         body["payment_success"].b()));
                } catch (const std::exception& error) {
                    log_exception("Process payment", error);
                    return message_response(500, "Could not process simulated payment.");
                }
            });

        CROW_ROUTE(app, "/api/bookings/<int>/cancel").methods(crow::HTTPMethod::POST)(
            [&bookings](const crow::request& request, int booking_id) {
                const auto body = crow::json::load(request.body);
                if (!body || !body.has("user_id")) {
                    return message_response(400, "JSON user_id is required.");
                }
                try {
                    return booking_response(
                        bookings.cancel(booking_id, body["user_id"].i()));
                } catch (const std::exception& error) {
                    log_exception("Cancel booking", error);
                    return message_response(500, "Could not cancel booking.");
                }
            });

        CROW_ROUTE(app, "/api/waiting-list").methods(crow::HTTPMethod::POST)(
            [&bookings](const crow::request& request) {
                const auto body = crow::json::load(request.body);
                if (!body || !body.has("seat_id") || !body.has("user_id")) {
                    return message_response(400, "JSON seat_id and user_id are required.");
                }
                try {
                    return booking_response(bookings.join_waiting_list(
                        body["seat_id"].i(), body["user_id"].i()));
                } catch (const std::exception& error) {
                    log_exception("Join waiting list", error);
                    return message_response(500, "Could not join waiting list.");
                }
            });

        CROW_ROUTE(app, "/api/admin/stats")([&pool, &workers, queue_capacity, demo_mode] {
            if (demo_mode) return message_response(404, "Not found");
            const auto db_connections_in_use = pool.in_use();
            auto lease = pool.acquire();
            pqxx::read_transaction transaction(lease.connection());
            const auto row = transaction.exec(
                "SELECT (SELECT count(*) FROM events) AS total_events, "
                "(SELECT count(*) FROM seats) AS total_seats, "
                "(SELECT count(*) FROM seats WHERE status='AVAILABLE') AS available_seats, "
                "(SELECT count(*) FROM seats WHERE status='RESERVED') AS reserved_seats, "
                "(SELECT count(*) FROM seats WHERE status='BOOKED') AS booked_seats, "
                "(SELECT count(*) FROM bookings WHERE status='CONFIRMED') AS successful_bookings, "
                "((SELECT count(*) FROM bookings WHERE status='FAILED') + "
                "(SELECT count(*) FROM audit_log WHERE event_type='booking_failed')) "
                "AS failed_bookings, "
                "(SELECT count(*) FROM bookings WHERE status='CANCELLED') AS cancelled_bookings, "
                "(SELECT count(*) FROM waiting_list WHERE status='WAITING') AS waiting_requests")
                                    .one_row();
            crow::json::wvalue result;
            result["total_events"] = row["total_events"].as<std::int64_t>();
            result["total_seats"] = row["total_seats"].as<std::int64_t>();
            result["available_seats"] = row["available_seats"].as<std::int64_t>();
            result["reserved_seats"] = row["reserved_seats"].as<std::int64_t>();
            result["booked_seats"] = row["booked_seats"].as<std::int64_t>();
            result["successful_bookings"] = row["successful_bookings"].as<std::int64_t>();
            result["failed_bookings"] = row["failed_bookings"].as<std::int64_t>();
            result["cancelled_bookings"] = row["cancelled_bookings"].as<std::int64_t>();
            result["waiting_requests"] = row["waiting_requests"].as<std::int64_t>();
            result["active_worker_threads"] = workers.active_workers();
            result["worker_count"] = workers.worker_count();
            result["request_queue_size"] = workers.queue_size();
            result["request_queue_peak_size"] = workers.peak_queue_size();
            result["request_queue_capacity"] = queue_capacity;
            result["database_connections_in_use"] = db_connections_in_use;
            result["database_connection_limit"] = pool.capacity();
            result["database_connections_peak"] = pool.peak_in_use();
            result["scheduling_policy"] = policy_name(workers.policy());
            return json_response(200, std::move(result));
        });

        CROW_ROUTE(app, "/api/admin/consistency")([&pool, demo_mode] {
            if (demo_mode) return message_response(404, "Not found");
            const auto report = check_database_consistency(pool);
            crow::json::wvalue result;
            result["duplicate_active_bookings"] = report.duplicate_active_bookings;
            result["duplicate_active_reservations"] = report.duplicate_active_reservations;
            result["seat_status_mismatches"] = report.seat_status_mismatches;
            result["expired_active_reservations"] = report.expired_active_reservations;
            result["cancelled_seat_conflicts"] = report.cancelled_seat_conflicts;
            result["consistent"] = report.consistent();
            return json_response(200, std::move(result));
        });

        CROW_ROUTE(app, "/api/admin/seats/<int>")([&pool, demo_mode](int seat_id) {
            if (demo_mode) return message_response(404, "Not found");
            auto lease = pool.acquire();
            pqxx::read_transaction transaction(lease.connection());
            const auto rows = database::execute(transaction, 
                "SELECT s.id, s.seat_label, s.status, "
                "(SELECT count(*) FROM bookings b WHERE b.seat_id=s.id "
                "AND b.status IN ('PENDING','CONFIRMED')) AS active_booking_count, "
                "(SELECT count(*) FROM waiting_list w WHERE w.seat_id=s.id "
                "AND w.status='WAITING') AS waiting_count "
                "FROM seats s WHERE s.id=$1", seat_id);
            if (rows.empty()) return message_response(404, "Seat was not found.");
            crow::json::wvalue result;
            result["id"] = rows[0]["id"].as<std::int64_t>();
            result["label"] = rows[0]["seat_label"].as<std::string>();
            result["status"] = rows[0]["status"].as<std::string>();
            result["active_booking_count"] =
                rows[0]["active_booking_count"].as<std::int64_t>();
            result["waiting_count"] = rows[0]["waiting_count"].as<std::int64_t>();
            return json_response(200, std::move(result));
        });

        CROW_ROUTE(app, "/api/admin/audit")([&pool, demo_mode] {
            if (demo_mode) return message_response(404, "Not found");
            auto lease = pool.acquire();
            pqxx::read_transaction transaction(lease.connection());
            const auto rows = transaction.exec(
                "SELECT id, event_type, details::text AS details, created_at "
                "FROM audit_log ORDER BY created_at DESC, id DESC LIMIT 50");
            crow::json::wvalue::list entries;
            for (const auto& row : rows) {
                crow::json::wvalue entry;
                entry["id"] = row["id"].as<std::int64_t>();
                entry["event_type"] = row["event_type"].as<std::string>();
                entry["details"] = row["details"].as<std::string>();
                entry["created_at"] = row["created_at"].as<std::string>();
                entries.emplace_back(std::move(entry));
            }
            crow::json::wvalue result;
            result["entries"] = std::move(entries);
            return json_response(200, std::move(result));
        });

        CROW_ROUTE(app, "/api/scheduling/demo")([&aging_interval, &workers, demo_mode] {
            if (demo_mode) return message_response(404, "Not found");
            const std::vector<ScheduledRequest> requests{
                {"R1", 2, 0, 10}, {"R2", 0, 0, 1}, {"R3", 1, 8, 1}};
            const auto result = compare_schedulers(
                requests, static_cast<int>(aging_interval.count()));
            crow::json::wvalue response;
            response["active_policy"] = policy_name(workers.policy());
            response["priority_basis"] =
                "Client booking priority (integer; higher is served first). Default is 0.";
            response["aging_interval_seconds"] = aging_interval.count();
            response["fcfs"] = schedule_json(result.fcfs);
            response["priority"] = schedule_json(result.priority);
            response["aging"] = schedule_json(result.aging);
            return json_response(200, std::move(response));
        });

        CROW_ROUTE(app, "/api/deadlock/demo")([demo_mode] {
            if (demo_mode) return message_response(404, "Not found");
            auto result = crow::json::load(run_deadlock_demonstration());
            return json_response(200, std::move(result));
        });

        app.bindaddr(address).port(port).multithreaded().run();
        return 0;
    } catch (const std::exception& error) {
        log_exception("Server startup/runtime", error);
        return 1;
    }
}
