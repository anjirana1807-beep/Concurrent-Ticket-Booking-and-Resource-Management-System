#pragma once

#include <pqxx/pqxx>

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <memory>
#include <queue>
#include <semaphore>
#include <string>
#include <vector>

class ConnectionPool {
public:
    class Lease {
    public:
        Lease() = default;
        Lease(ConnectionPool* owner, pqxx::connection* connection);
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();
        pqxx::connection& connection() const;

    private:
        void release();
        ConnectionPool* owner_ = nullptr;
        pqxx::connection* connection_ = nullptr;
    };

    ConnectionPool(std::string connection_string, std::size_t capacity);
    Lease acquire();
    std::size_t capacity() const;
    std::size_t in_use() const;
    std::size_t peak_in_use() const;

private:
    friend class Lease;
    void put_back(pqxx::connection* connection);

    std::vector<std::unique_ptr<pqxx::connection>> connections_;
    std::queue<pqxx::connection*> available_;
    mutable std::mutex mutex_;
    std::counting_semaphore<> permits_;
    std::size_t in_use_ = 0;
    std::size_t peak_in_use_ = 0;
};
