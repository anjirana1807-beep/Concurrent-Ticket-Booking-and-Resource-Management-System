#!/usr/bin/env python3
"""Send twenty simultaneous booking requests for the same available seat."""

import argparse
import concurrent.futures
import os
import sys
import threading
import time
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
    parser.add_argument("--base-url", default=os.getenv("API_BASE_URL", "http://127.0.0.1:18080"))
    parser.add_argument("--event-id", type=int, help="Defaults to the first event.")
    parser.add_argument("--seat-label", default="A1")
    parser.add_argument("--users", type=int, default=20)
    args = parser.parse_args()
    if args.users < 2:
        parser.error("--users must be at least 2")
    base = args.base_url.rstrip("/")

    _, events_body = request_json("GET", f"{base}/api/events")
    if not events_body["events"]:
        raise RuntimeError("No events found; apply database/seed.sql first.")
    event_id = args.event_id or events_body["events"][0]["id"]
    _, seats_body = request_json("GET", f"{base}/api/events/{event_id}/seats")
    seat = next((item for item in seats_body["seats"] if item["label"] == args.seat_label), None)
    if seat is None:
        raise RuntimeError(f"Seat {args.seat_label} was not found for event {event_id}.")
    if seat["status"] != "AVAILABLE":
        raise RuntimeError(
            f"Seat {args.seat_label} is {seat['status']}; cancel its active booking "
            "or choose another available seat."
        )

    unique = uuid.uuid4().hex[:12]
    users = []
    for index in range(args.users):
        _, user = request_json(
            "POST",
            f"{base}/api/users",
            json={"name": f"Concurrent Tester {index + 1}", "email": f"race-{unique}-{index}@example.test"},
        )
        users.append(user["user_id"])

    barrier = threading.Barrier(args.users)

    def book(user_id):
        barrier.wait(timeout=20)
        try:
            status, body = request_json(
                "POST",
                f"{base}/api/bookings",
                json={"seat_id": seat["id"], "user_id": user_id, "payment_success": True},
            )
            return status, body
        except Exception as error:  # Keep a failed request visible in the aggregate.
            return 0, {"success": False, "message": str(error)}

    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.users) as executor:
        results = list(executor.map(book, users))
    elapsed = time.monotonic() - started

    successes = [body for status, body in results if status == 200 and body.get("success")]
    rejected_results = [
        (status, body) for status, body in results
        if not (status == 200 and body.get("success"))
    ]
    rejected = len(rejected_results)
    status, seat_result = request_json("GET", f"{base}/api/admin/seats/{seat['id']}")
    _, consistency = request_json("GET", f"{base}/api/admin/consistency")
    _, metrics = request_json("GET", f"{base}/api/admin/stats")

    print(f"Requests: {len(results)} | successful: {len(successes)} | rejected/errors: {rejected}")
    print(f"Elapsed: {elapsed:.2f}s | seat: {seat_result['label']} ({seat_result['status']})")
    print(f"Active bookings for seat: {seat_result['active_booking_count']}")
    print(f"Peak DB connections: {metrics['database_connections_peak']} / "
          f"{metrics['database_connection_limit']} | peak queued: "
          f"{metrics['request_queue_peak_size']}")
    print(f"Database consistency: {consistency}")
    for index, (http_status, body) in enumerate(results, start=1):
        print(f"  request {index:02d}: HTTP {http_status} | {body.get('message', 'no response')}")

    if len(successes) != 1 or seat_result["active_booking_count"] != 1:
        print("FAIL: expected exactly one successful and active booking.", file=sys.stderr)
        return 1
    expected_conflict = "Sorry, this seat is already booked."
    if len(rejected_results) != args.users - 1 or any(
        status != 409 or body.get("message") != expected_conflict
        for status, body in rejected_results
    ):
        print("FAIL: losing requests did not all receive the expected seat-conflict response.",
              file=sys.stderr)
        return 1
    if not consistency["consistent"]:
        print("FAIL: database consistency check reported a violation.", file=sys.stderr)
        return 1
    if metrics["database_connections_peak"] > metrics["database_connection_limit"]:
        print("FAIL: observed DB connection usage exceeded its configured limit.", file=sys.stderr)
        return 1
    if metrics["request_queue_peak_size"] > metrics["request_queue_capacity"]:
        print("FAIL: observed request queue usage exceeded its configured capacity.", file=sys.stderr)
        return 1
    if metrics["worker_count"] < 2:
        print("FAIL: same-seat concurrency test requires at least two workers.", file=sys.stderr)
        return 1
    print("PASS: one active booking; no duplicate booking was created.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
