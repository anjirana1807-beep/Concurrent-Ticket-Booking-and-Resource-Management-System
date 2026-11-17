#!/usr/bin/env python3
"""Verify that demo mode hides diagnostics but keeps event and seat reads available."""

import argparse
import uuid

import requests


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:18080")
    args = parser.parse_args()
    base = args.base_url.rstrip("/")

    health_response = requests.get(f"{base}/api/health", timeout=10)
    health_response.raise_for_status()
    health = health_response.json()
    if health != {"status": "ok", "demo_mode": True}:
        raise AssertionError(
            "Demo health response must contain only status and demo_mode=true; "
            f"received {health!r}"
        )

    events_response = requests.get(f"{base}/api/events", timeout=10)
    events_response.raise_for_status()
    events = events_response.json()["events"]
    if not events:
        raise AssertionError("No events found; apply database/seed.sql first.")

    seats_response = requests.get(
        f"{base}/api/events/{events[0]['id']}/seats", timeout=10
    )
    seats_response.raise_for_status()
    seats = seats_response.json().get("seats")
    if seats is None:
        raise AssertionError("Seat endpoint did not return a seats collection.")

    restricted_paths = (
        "/api/admin/stats",
        "/api/admin/consistency",
        "/api/admin/seats/1",
        "/api/admin/audit",
        "/api/scheduling/demo",
        "/api/deadlock/demo",
    )
    for path in restricted_paths:
        response = requests.get(f"{base}{path}", timeout=10)
        if response.status_code != 404:
            raise AssertionError(f"{path} returned HTTP {response.status_code}, expected 404")

    available = next((seat for seat in seats if seat["status"] == "AVAILABLE"), None)
    if available is None:
        raise AssertionError("No available seat for the demo-mode booking check.")
    unique = uuid.uuid4().hex[:12]
    user_response = requests.post(
        f"{base}/api/users",
        json={"name": "Demo Mode Tester", "email": f"demo-{unique}@example.test"},
        timeout=10,
    )
    user_response.raise_for_status()
    user = user_response.json()
    booking_response = requests.post(
        f"{base}/api/bookings",
        json={
            "seat_id": available["id"],
            "user_id": user["user_id"],
            "payment_success": True,
        },
        timeout=30,
    )
    booking_response.raise_for_status()
    if not booking_response.json().get("success"):
        raise AssertionError(f"Demo-mode booking was rejected: {booking_response.json()}")
    seats_after = requests.get(
        f"{base}/api/events/{events[0]['id']}/seats", timeout=10
    )
    seats_after.raise_for_status()
    booked_seat = next(
        (seat for seat in seats_after.json()["seats"] if seat["id"] == available["id"]),
        None,
    )
    if booked_seat is None or booked_seat["status"] != "BOOKED":
        raise AssertionError(f"Seat status did not reflect the booking: {booked_seat}")

    print("PASS: demo health is minimal; events/seats and booking work; diagnostics return 404.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
