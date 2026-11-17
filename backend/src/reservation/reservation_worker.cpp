#include "reservation/reservation_worker.hpp"

#include <chrono>
#include <exception>
#include <iostream>

ReservationExpiryWorker::ReservationExpiryWorker(BookingService& bookings)
    : thread_([&bookings](std::stop_token stop_token) {
          while (!stop_token.stop_requested()) {
              try {
                  const auto expired = bookings.expire_reservations();
                  if (expired > 0) {
                      std::cout << "Expired and released " << expired
                                << " reservation(s)\n";
                  }
              } catch (const std::exception& error) {
                  std::cerr << "Reservation expiry worker: " << error.what() << '\n';
              }
              for (int i = 0; i < 10 && !stop_token.stop_requested(); ++i) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }
          }
      }) {}

