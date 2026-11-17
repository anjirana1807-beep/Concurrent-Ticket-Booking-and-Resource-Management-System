#!/usr/bin/env python3
"""Exercise payment rollback, expiry, cancellation, waiting list and parallel seats."""

import argparse
import concurrent.futures
import os
import sys
import time
import uuid

import requests


def call(method, base, path, **kwargs):
    response = requests.request(method, base.rstrip("/") + path, timeout=30, **kwargs)
    try:
        payload = response.json()
    except ValueError as error:
        raise RuntimeError(f"{path} returned non-JSON HTTP {response.status_code}") from error
    return response.status_code, payload


def create_user(base, label):
    token = uuid.uuid4().hex[:12]
    _, result = call(
        "POST", base, "/api/users",
        json={"name": label, "email": f"{label.lower().replace(' ', '-')}-{token}@example.test"},
    )
    return result["user_id"]


def available_or_any(seats, label):
    seat = next((item for item in seats if item["label"] == label), None)
    if seat is None:
        raise RuntimeError(f"Required seeded test seat {label} was not found.")
    if seat["status"] != "AVAILABLE":
        raise RuntimeError(f"Required test seat {label} is {seat['status']}; reset/cancel it first.")
    return seat


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=os.getenv("API_BASE_URL", "http://127.0.0.1:18080"))
    parser.add_argument("--expiry-wait-seconds", type=int, default=int(os.getenv("EXPIRY_WAIT_SECONDS", "125")))
    args = parser.parse_args()
    base = args.base_url.rstrip("/")
    _, events_response = call("GET", base, "/api/events")
    if not events_response["events"]:
        raise RuntimeError("No events found; apply database/seed.sql first.")
    event_id = events_response["events"][0]["id"]
    _, seat_response = call("GET", base, f"/api/events/{event_id}/seats")
    seats = seat_response["seats"]

    # Payment failure must leave neither an active booking nor an occupied seat.
    payment_seat = available_or_any(seats, "A2")
    payment_user = create_user(base, "Payment Failure")
    status, payment = call(
        "POST", base, "/api/bookings",
        json={"seat_id": payment_seat["id"], "user_id": payment_user, "payment_success": False},
    )
    assert status == 409 and not payment["success"], payment
    _, payment_state = call("GET", base, f"/api/admin/seats/{payment_seat['id']}")
    assert payment_state["status"] == "AVAILABLE"
    assert payment_state["active_booking_count"] == 0
    print("PASS: simulated payment failure rolled back and released the seat.")

    # Reservation expiry is configured by the server; use a short TTL in .env for this test.
    expiry_seat = available_or_any(seats, "A4")
    expiry_user = create_user(base, "Expiry Tester")
    _, reservation = call(
        "POST", base, "/api/reservations",
        json={"seat_id": expiry_seat["id"], "user_id": expiry_user},
    )
    assert reservation["success"], reservation
    print(f"Waiting {args.expiry_wait_seconds}s for reservation expiry...")
    time.sleep(args.expiry_wait_seconds)
    _, expiry_state = call("GET", base, f"/api/admin/seats/{expiry_seat['id']}")
    assert expiry_state["status"] == "AVAILABLE", expiry_state
    print("PASS: expiry worker released the temporary reservation.")

    # Cancellation releases the seat and atomically offers it to the next waiter.
    wait_seat = available_or_any(seats, "A3")
    first_user = create_user(base, "Cancellation Owner")
    next_user = create_user(base, "Waiting User")
    _, booking = call(
        "POST", base, "/api/bookings",
        json={"seat_id": wait_seat["id"], "user_id": first_user, "payment_success": True},
    )
    assert booking["success"], booking
    _, joined = call(
        "POST", base, "/api/waiting-list",
        json={"seat_id": wait_seat["id"], "user_id": next_user},
    )
    assert joined["success"], joined
    _, cancelled = call(
        "POST", base, f"/api/bookings/{booking['booking_id']}/cancel",
        json={"user_id": first_user},
    )
    assert cancelled["success"], cancelled
    _, offers = call("GET", base, f"/api/users/{next_user}/reservations")
    offer = next((item for item in offers["reservations"] if item["seat_id"] == wait_seat["id"]), None)
    assert offer is not None, "Next waiter was not offered a reservation."
    _, accepted = call(
        "POST", base, f"/api/reservations/{offer['id']}/pay",
        json={"payment_success": True},
    )
    assert accepted["success"], accepted
    _, wait_state = call("GET", base, f"/api/admin/seats/{wait_seat['id']}")
    assert wait_state["status"] == "BOOKED" and wait_state["active_booking_count"] == 1
    print("PASS: cancellation released the seat and safely offered it to the next waiter.")

    # Concurrent requests for independent seats should all be able to complete.
    different_seats = [
        available_or_any(seats, label) for label in ("A5", "A6")
    ]
    def book_distinct(item):
        index, seat = item
        user = create_user(base, f"Different Seat {index}")
        return call(
            "POST", base, "/api/bookings",
            json={"seat_id": seat["id"], "user_id": user, "payment_success": True},
        )

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(different_seats)) as executor:
        results = list(executor.map(book_distinct, enumerate(different_seats, start=1)))
    assert all(status == 200 and result["success"] for status, result in results), results
    _, consistency = call("GET", base, "/api/admin/consistency")
    assert consistency["consistent"], consistency
    print("PASS: parallel requests for different seats completed; consistency check is clean.")
    print("PASS: all lifecycle tests completed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError, requests.RequestException) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
