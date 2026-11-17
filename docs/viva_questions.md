# Viva preparation: questions and short answers

### 1. What is the race condition in this project?
Two independent requests can both observe a seat as available and attempt to book
it. Without synchronization both might insert a booking. Here PostgreSQL row
locking serializes transactions for the seat.

### 2. Why is the request queue not sufficient to prevent duplicates?
The queue controls how much work the process accepts and when workers start. Another
server process or future API path could bypass that queue. Database transactions,
`SELECT FOR UPDATE`, and a unique partial index provide the consistency boundary.

### 3. What does `SELECT FOR UPDATE` do here?
It locks the selected seat row until transaction commit/rollback. A competing
transaction trying to lock the same row waits, then reads its committed state.

### 4. What is the final duplicate-booking safeguard?
`bookings_one_active_per_seat` is a PostgreSQL unique partial index covering
`PENDING` and `CONFIRMED` statuses. `FAILED` and `CANCELLED` rows remain as history
without blocking a later booking.

### 5. What does the semaphore control?
It limits the number of threads that can lease a database connection simultaneously
to `DB_CONNECTION_LIMIT`. It does not lock a seat or replace the transaction.

### 6. What happens when the request queue is full?
The API rejects new booking work with HTTP 503, rather than silently dropping it or
growing memory without bound. The client can retry.

### 7. Explain a temporary reservation.
The seat moves `AVAILABLE -> RESERVED` with an expiry timestamp. Payment success
moves it to `BOOKED`; payment failure or expiry releases it to `AVAILABLE`. Updates
to reservation, booking/payment history, and seat state are committed transactionally.

### 8. How is the waiting list processed?
When a seat is released, the transaction locks the earliest waiting row using
`FOR UPDATE SKIP LOCKED`, creates one expiring reservation for that user and marks
the request `OFFERED`. Other release workers cannot offer the same row.

### 9. Compare FCFS, priority and aging.
FCFS serves arrival order. Priority serves a larger integer priority first. Aging
gradually raises the effective priority of jobs that wait, reducing starvation. The
simulation reports order and waiting time; the same database booking checks apply
regardless of scheduler.

### 10. Is two users choosing one seat a deadlock?
No. They contend for one lock; one waits, then sees the seat booked. A deadlock
requires a cycle, such as thread 1 holding A while waiting for B, and thread 2
holding B while waiting for A.

### 11. How does consistent lock ordering prevent deadlock?
Every thread acquires the shared resources in the same global order, e.g. resource 0
before resource 1. A circular wait cannot form if all participants obey that order.

### 12. How can two computers share one booking state?
Both browsers send HTTP requests to the same Crow server LAN address. Only the
server talks to PostgreSQL, so both clients observe the same database transaction
and seat state.

