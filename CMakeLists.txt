cmake_minimum_required(VERSION 3.21)
project(concurrent_ticket_booking LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Crow CONFIG REQUIRED)
find_package(libpqxx 7.10 CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(ticket_server
    backend/main.cpp
    backend/src/database/connection_pool.cpp
    backend/src/concurrency/worker_pool.cpp
    backend/src/concurrency/deadlock_demo.cpp
    backend/src/booking/booking_service.cpp
    backend/src/reservation/reservation_worker.cpp
    backend/src/scheduler/scheduler.cpp
    backend/src/monitoring/monitoring.cpp
)
target_include_directories(ticket_server PRIVATE backend/include)
if(TARGET libpqxx::pqxx)
    set(PQXX_TARGET libpqxx::pqxx)
elseif(TARGET pqxx)
    set(PQXX_TARGET pqxx)
else()
    message(FATAL_ERROR "libpqxx package did not provide a known CMake target")
endif()
target_link_libraries(ticket_server PRIVATE Crow::Crow ${PQXX_TARGET} Threads::Threads)

if(MSVC)
    target_compile_options(ticket_server PRIVATE /W4 /permissive-)
else()
    target_compile_options(ticket_server PRIVATE -Wall -Wextra -Wpedantic)
endif()
