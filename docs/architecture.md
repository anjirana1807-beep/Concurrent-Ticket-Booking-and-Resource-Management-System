# Architecture and concurrency notes

## Request path

```text
Browser (HTML/CSS/JS)
  -> Crow REST API on the server computer
    -> bounded request queue / fixed worker pool
      -> BookingService
        -> semaphore-limited libpqxx connection pool
          -> PostgreSQL transaction and constraints
```

The frontend is served by Crow, so both computers can visit the same
`http://<server-LAN-IP>:<port>` URL. Frontend API requests use relative paths and
therefore stay on the page's origin. Do not configure clients to connect directly
to PostgreSQL.

## Booking consistency

The queued request is workload management, not the consistency lock. For a booking,
`BookingService::book` opens a database transaction, locks the seat row using
`SELECT ... FOR UPDATE`, checks `AVAILABLE`, creates its reservation and booking,
records the simulated payment, and updates seat state before commit. Competing
transactions for the same seat serialize at the row lock. The partial unique index
`bookings_one_active_per_seat` is the final defense if another code path fails to
follow that protocol. A bounded set of C++ seat mutexes also serializes same-seat
work within this process; PostgreSQL remains the source of truth across multiple
processes and for requests that do not share that mutex. Failed or cancelled rows
do not occupy that unique active booking slot. Active reservations are also
protected by a unique partial index, so only one reservation in `ACTIVE` state can
exist per seat; the expiry worker changes expired rows out of that state.

## Resource controls

- A `std::counting_semaphore` provides at most `DB_CONNECTION_LIMIT` simultaneous
  connection leases. Each libpqxx connection is used by one lease at a time.
- `WorkerPool` has `WORKER_COUNT` workers and a bounded queue. If the queue is full,
  a request receives HTTP 503; increasing queue size does not improve correctness.
- Queue scheduling can be configured as `FCFS`, `PRIORITY` or `AGING`. Larger
  integer request priorities are served earlier. Aging adds one priority point per
  `AGING_INTERVAL_SECONDS` spent waiting. These choices affect service order only;
  every worker still uses the same database checks and locks.

## Booking lifecycle

```text
AVAILABLE -> RESERVED -> BOOKED
                  |         |
                  v         v
             AVAILABLE <- CANCELLED
```

Reservations expire according to `RESERVATION_TTL_SECONDS`. The expiry worker
locks expired reservation rows, releases the seat transactionally, and offers the
seat to the earliest waiting request. An offer is another short-lived reservation;
the waiting user can find it in the frontend and simulate payment. A failed payment
also releases the seat and attempts the next offer in the same transaction.

## Scheduling and deadlock exercises

The scheduler demo uses arrival time and integer priority (larger means more
important). It returns service order and time spent waiting for a fixed sample with
different arrival and service times under
FCFS, non-preemptive priority, and priority with aging. Production booking
consistency remains database-driven and scheduling is not a lock.

The separate deadlock endpoint coordinates two threads that each hold one timed
resource and then request the other in opposite order. Timed attempts expose the
circular-wait risk without leaving a hung server. A second run acquires resource 0
before resource 1 in every thread. Mere contention is one thread waiting for a
resource; deadlock requires a cycle of waits.

## Monitoring

`/api/admin/stats` reports event/seat/booking/waiting-list totals, active and
configured workers, current and peak queue length, and current/peak DB leases.
`audit_log`
records success, failed attempts, payment failures, expiry, cancellation and
waiting-list offers. `/api/admin/consistency` reports duplicate active bookings,
seat-state mismatches, expired-but-active reservations and cancelled-seat conflicts.
