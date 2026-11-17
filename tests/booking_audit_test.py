#!/usr/bin/env python3
"""Verify that a failed payment releases the seat and commits an audit record."""

import argparse
import json
import uuid

import requests


def request_json(method, url, **kwargs):
    response = requests.request(method, url, timeout=30, **kwargs)
    try:
        body = response.json()
    except ValueError as error:
        raise RuntimeError(f"{url} returned non-JSON HTTP {response.status_code}") from error
    return response.status_code, body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:18080")
    parser.add_argument("--seat-label", default="A1")
    args = parser.parse_args()
    base = args.base_url.rstrip("/")

    _, events_body = request_json("GET", f"{base}/api/events")
    if not events_body["events"]:
        raise RuntimeError("No events found; apply database/seed.sql first.")
    event_id = events_body["events"][0]["id"]
    _, seats_body = request_json("GET", f"{base}/api/events/{event_id}/seats")
    seat = next((item for item in seats_body["seats"] if item["label"] == args.seat_label), None)
    if seat is None:
        raise RuntimeError(f"Seat {args.seat_label} was not found for event {event_id}.")
    if seat["status"] != "AVAILABLE":
        raise RuntimeError(f"Seat {args.seat_label} is {seat['status']}; choose an available seat.")

    unique = uuid.uuid4().hex[:12]
    status, user = request_json(
        "POST",
        f"{base}/api/users",
        json={"name": "Audit Failure Tester", "email": f"audit-{unique}@example.test"},
    )
    if status != 200:
        raise AssertionError(f"User creation returned HTTP {status}: {user}")

    status, booking = request_json(
        "POST",
        f"{base}/api/bookings",
        json={"seat_id": seat["id"], "user_id": user["user_id"], "payment_success": False},
    )
    if status != 409 or booking.get("success") or not booking.get("booking_id"):
        raise AssertionError(f"Expected a failed-payment conflict response, got {status}: {booking}")

    status, seat_state = request_json("GET", f"{base}/api/admin/seats/{seat['id']}")
    if status != 200 or seat_state["status"] != "AVAILABLE":
        raise AssertionError(f"Failed payment did not release the seat: {status}, {seat_state}")

    _, audit = request_json("GET", f"{base}/api/admin/audit")
    found = any(
        entry["event_type"] == "payment_failed"
        and json.loads(entry["details"]).get("booking_id") == booking["booking_id"]
        for entry in audit["entries"]
    )
    if not found:
        raise AssertionError(f"No payment_failed audit record for booking {booking['booking_id']}.")

    status, error_body = request_json(
        "POST",
        f"{base}/api/bookings",
        json={"seat_id": seat["id"], "user_id": -1, "payment_success": True},
    )
    if status != 500:
        raise AssertionError(
            f"Expected a database failure for a missing user, got {status}: {error_body}"
        )
    _, seat_state = request_json("GET", f"{base}/api/admin/seats/{seat['id']}")
    if seat_state["status"] != "AVAILABLE" or seat_state["active_booking_count"] != 0:
        raise AssertionError(f"Failed transaction left the seat booked: {seat_state}")
    _, audit = request_json("GET", f"{base}/api/admin/audit")
    rollback_audited = any(
        entry["event_type"] == "booking_failed"
        and (details := json.loads(entry["details"])).get("seat_id") == seat["id"]
        and details.get("user_id") == -1
        and details.get("reason") == "database_error"
        for entry in audit["entries"]
    )
    if not rollback_audited:
        raise AssertionError("Database failure was not audited after its transaction rollback.")

    _, deadlock = request_json("GET", f"{base}/api/deadlock/demo")
    if deadlock.get("unsafe_order_timeouts", 0) < 1 or not deadlock.get(
        "safe_order_completed"
    ):
        raise AssertionError(
            "Deadlock demonstration did not report timeout and safe completion: "
            f"{deadlock}"
        )

    print("PASS: payment rollback/audit and timed deadlock handling checks succeeded.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
