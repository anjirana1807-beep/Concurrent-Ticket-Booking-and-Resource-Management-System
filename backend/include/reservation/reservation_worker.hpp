#pragma once

#include "booking/booking_service.hpp"

#include <thread>

class ReservationExpiryWorker {
public:
    explicit ReservationExpiryWorker(BookingService& bookings);
    ReservationExpiryWorker(const ReservationExpiryWorker&) = delete;
    ReservationExpiryWorker& operator=(const ReservationExpiryWorker&) = delete;

private:
    std::jthread thread_;
};

