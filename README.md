**# Concurrent Ticket Booking and Resource Management**

A C++20/Crow and PostgreSQL teaching project demonstrating safe seat booking,
bounded request processing, temporary reservations, waiting-list offers,
scheduling policies, audit events, and consistency checks.

**## Architecture and modules**

Crow serves the HTML, CSS, JavaScript, and REST API from one origin on port
18080\. The browser calls relative API paths. Crow uses a bounded C++ worker pool
for direct booking requests; the booking service uses a PostgreSQL connection
pool and transactional seat-row locks. PostgreSQL is a private backend dependency
and is never a browser or tunnel target.

\- **\*\*Users and events:\*\*** create/select users, list events, and read seat status.
\- **\*\*Booking:\*\*** serialize same-seat requests with a process mutex and PostgreSQL
  \`SELECT ... FOR UPDATE\`, then commit the reservation, booking, payment outcome,
  seat state, and audit event together. A partial unique index is the final
  duplicate-booking guard.
\- **\*\*Reservations:\*\*** temporary reservations expire in a background worker; expiry,
  cancellation, and payment failure release seats and offer them to waiting users
  transactionally.
\- **\*\*Scheduling/resources:\*\*** producer-consumer worker queue, FCFS by default,
  optional priority/aging policies, fixed worker count, and semaphore-limited
  PostgreSQL connections.
\- **\*\*Monitoring/failure handling:\*\*** audit log, consistency checks, bounded retry
  for PostgreSQL deadlock, serialization, and lock-timeout errors, plus an
  isolated timed-mutex deadlock demonstration.

The implementation demonstrates mutexes, threads, a condition-variable queue,
semaphore-based resource limits, FCFS scheduling, PostgreSQL ACID transactions,
row locks, keys/constraints, rollback, and audit records. A failed simulated
payment is recorded as a failed booking/payment and releases its seat in the
same transaction; a transaction-level booking error is audited separately after
the failed transaction is rolled back.

**## What is included**

\- **\*\*Booking:\*\*** list seeded events and seats; create/select a user; book and cancel.
\- **\*\*Concurrency:\*\*** bounded C++ worker queue, configurable worker count and priority
  policy, connection pool limited by a counting semaphore, PostgreSQL transactions,
  seat-row \`SELECT ... FOR UPDATE\`, and a partial unique index on active bookings.
\- **\*\*Lifecycle:\*\*** temporary reservations, background expiry, simulated payment and
  rollback, cancellation, and safe next-waiter offers.
\- **\*\*OS demonstrations:\*\*** FCFS, priority and aging order/wait-time comparison; a
  separate bounded deadlock-risk demonstration and consistent lock-order solution.
\- **\*\*Monitoring:\*\*** counts, current/peak pool and queue usage, recent audit events, and database
  invariants.
\- **\*\*Testing:\*\*** Python \`requests\`/\`concurrent.futures\` scripts.

**## Quick start (Windows)**

See [docs/setup.md]\(docs/setup.md) for installing dependencies, database setup,
build/run instructions, firewall configuration and the two-computer demo.

1\. Install MSYS2 UCRT64 GCC, CMake, Ninja, PostgreSQL, and Python.
2\. Install Crow and libpqxx in the UCRT64 prefix if they are not already installed;
   see [docs/setup.md](docs/setup.md) for the exact build and dependency commands.
3\. Create a PostgreSQL database and user, then apply \`database/schema.sql\` followed
   by \`database/seed.sql\`.
4\. Copy \`.env.example\` to \`.env\`, update the database credentials and load the
   variables into the shell.
5\. Configure/build in \`build-ucrt64\` and run the executable from this project root.
6\. Open \`[http://127.0.0.1:18080](http://127.0.0.1:18080)\` (or the configured port).

**## Main endpoints**

\| Method | Path | Purpose |
\|---|---|---|
\| GET | \`/api/health\` | Health and demo-mode status |
\| GET | \`/api/events\` | List events |
\| GET | \`/api/events/{event_id}/seats\` | List seat state |
\| POST | \`/api/users\` | Create or select a user by email |
\| GET | \`/api/users/{user_id}/bookings\` | List a user's bookings |
\| GET | \`/api/users/{user_id}/reservations\` | List active reservations/offers |
\| POST | \`/api/bookings\` | Queue and atomically book/pay for a seat |
\| POST | \`/api/reservations\` | Temporarily reserve a seat |
\| POST | \`/api/reservations/{id}/pay\` | Complete/fail simulated payment |
\| POST | \`/api/bookings/{id}/cancel\` | Cancel a confirmed booking |
\| POST | \`/api/waiting-list\` | Join a seat's waiting list |
\| GET | \`/api/admin/stats\` | System counters and resource usage |
\| GET | \`/api/admin/consistency\` | Database invariant checks |
\| GET | \`/api/admin/seats/{seat_id}\` | Seat state and active booking count |
\| GET | \`/api/admin/audit\` | Most recent 50 audit records |
\| GET | \`/api/scheduling/demo\` | Compare scheduling policies |
\| GET | \`/api/deadlock/demo\` | Run isolated deadlock demonstration |

Booking request body: \`{"seat_id": 1, "user_id": 1, "payment_success": true,
"priority": 0}\`. A non-available seat returns HTTP 409 with the message
**\*\*“Sorry, this seat is already booked.”\*\*** The booking endpoint treats omitted
\`payment_success\` as a simulated outcome using \`PAYMENT_FAILURE_RATE\`; the UI and
tests set the outcome explicitly.

**## Test**

Install the only test dependency with \`python -m pip install requests\`.

\`\`\`powershell
python tests\concurrent_booking_test.py --seat-label A1
python tests\concurrent_booking_test.py --seat-label A1 --users 2
python tests\other_tests.py
python tests\booking_audit_test.py --seat-label B1
\`\`\`

The same-seat test requires an available A1; by default it creates 20 unique users,
synchronizes their start, and checks that exactly one confirmed active booking remains.
Use \`--users 2\` for a synchronized two-user race. The
lifecycle test uses A2-A6. For a quick reservation-expiry test, set
\`RESERVATION_TTL_SECONDS=3\` and run:

\`\`\`powershell
$env:EXPIRY_WAIT_SECONDS = "5"
python tests\other_tests.py
\`\`\`

The scripts create test users and leave successful bookings in the database. Reset
only a disposable demo database between runs, or use other available seats.

**## Temporary public demo**

Set \`DEMO_MODE=true\` before starting the server. It disables the admin stats,
consistency, seat-diagnostic, audit, scheduler-demo, and deadlock-demo endpoints
with HTTP 404, and hides the admin panel. It also binds Crow to
\`127.0.0.1:18080\`; \`SERVER_ADDRESS\` cannot override that loopback bind in demo
mode. Run the separate demo-mode check against that instance:

\`\`\`powershell
python tests\demo_mode_test.py --base-url [http://127.0.0.1:18080](http://127.0.0.1:18080)
\`\`\`

In another PowerShell window, after installing Cloudflare \`cloudflared\`, create
the temporary tunnel:

\`\`\`powershell
cloudflared tunnel --url [http://127.0.0.1:18080](http://127.0.0.1:18080)
\`\`\`

Open the generated HTTPS URL on the laptop and phones. Stop the tunnel and server
after the demonstration. Keep PostgreSQL on localhost and never point the tunnel
at PostgreSQL.

**## Limits and safety notes**

This is a college demonstration, not production payment/authentication software.
There is no authentication or authorization: \`DEMO_MODE\` only disables the listed
diagnostic endpoints. Public visitors can still create users and use booking,
reservation, cancellation, and waiting-list APIs. A Quick Tunnel URL is public,
not access control. Use an isolated disposable database with non-sensitive data;
do not expose PostgreSQL or its port to the internet, and do not use production
credentials or data.