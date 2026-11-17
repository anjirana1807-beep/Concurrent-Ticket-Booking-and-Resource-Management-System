#include "database/connection_pool.hpp"

#include <stdexcept>
#include <utility>

ConnectionPool::Lease::Lease(ConnectionPool* owner, pqxx::connection* connection)
    : owner_(owner), connection_(connection) {}

ConnectionPool::Lease::Lease(Lease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      connection_(std::exchange(other.connection_, nullptr)) {}

ConnectionPool::Lease& ConnectionPool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        connection_ = std::exchange(other.connection_, nullptr);
    }
    return *this;
}

ConnectionPool::Lease::~Lease() {
    release();
}

pqxx::connection& ConnectionPool::Lease::connection() const {
    if (!connection_) {
        throw std::logic_error("Database connection lease is empty");
    }
    return *connection_;
}

void ConnectionPool::Lease::release() {
    if (owner_ && connection_) {
        owner_->put_back(connection_);
        owner_ = nullptr;
        connection_ = nullptr;
    }
}

ConnectionPool::ConnectionPool(std::string connection_string, std::size_t capacity)
    : permits_(static_cast<std::ptrdiff_t>(capacity)) {
    if (capacity == 0) {
        throw std::invalid_argument("DB_CONNECTION_LIMIT must be greater than zero");
    }
    connections_.reserve(capacity);
    for (std::size_t i = 0; i < capacity; ++i) {
        auto connection = std::make_unique<pqxx::connection>(connection_string);
        available_.push(connection.get());
        connections_.push_back(std::move(connection));
    }
}

ConnectionPool::Lease ConnectionPool::acquire() {
    permits_.acquire();
    std::lock_guard lock(mutex_);
    if (available_.empty()) {
        permits_.release();
        throw std::logic_error("Connection pool semaphore and queue are inconsistent");
    }
    auto* connection = available_.front();
    available_.pop();
    ++in_use_;
    if (in_use_ > peak_in_use_) peak_in_use_ = in_use_;
    return Lease(this, connection);
}

std::size_t ConnectionPool::capacity() const {
    return connections_.size();
}

std::size_t ConnectionPool::in_use() const {
    std::lock_guard lock(mutex_);
    return in_use_;
}

std::size_t ConnectionPool::peak_in_use() const {
    std::lock_guard lock(mutex_);
    return peak_in_use_;
}

void ConnectionPool::put_back(pqxx::connection* connection) {
    {
        std::lock_guard lock(mutex_);
        available_.push(connection);
        --in_use_;
    }
    permits_.release();
}
