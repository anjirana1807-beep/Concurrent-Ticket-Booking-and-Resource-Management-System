# Setup and two-computer demonstration (Windows)

## Requirements

- Windows 10/11, MSYS2 UCRT64 GCC (C++20), CMake 3.21+, Ninja, and Git.
- PostgreSQL server and command-line tools (`psql`).
- Python 3 and `requests`.
- Crow and libpqxx 7.10 or later installed in the MSYS2 UCRT64 prefix
  (`C:\msys64\ucrt64`).
- Cloudflare `cloudflared` only for the optional temporary public demo.

## Architecture and demonstrated concepts

Crow serves `frontend/index.html`, CSS, JavaScript, and the REST API from the same
origin on port 18080. The browser uses relative API paths. Crow's booking handler
submits work to a bounded producer-consumer queue; the worker pool uses FCFS by
default. `BookingService` serializes same-process access with seat mutexes and
uses PostgreSQL transactions with `SELECT ... FOR UPDATE` as the authoritative
cross-process lock. Partial unique indexes provide final protection against
duplicate active bookings and reservations.

- **Users/events:** create or select users, list events, and read seat status.
- **Booking:** one transaction updates reservation, booking, payment, seat, and
  audit state. Failed simulated payment releases the seat and commits the failed
  outcome; a database transaction error is rolled back and recorded by a separate
  audit transaction. Deadlock, serialization, and lock-timeout SQL errors receive
  at most three booking attempts.
- **Reservations:** a background expiry thread releases expired seats; cancellation
  and waiting-list offers use database transactions and row locks.
- **Scheduling/resources:** fixed worker threads, bounded queue, condition
  variable, optional priority/aging policies, and a semaphore-limited connection
  pool. FCFS remains the default.
- **Monitoring/demos:** consistency queries, audit log, scheduler comparison, and
  a timed-mutex deadlock demonstration are available outside demo mode.

The code demonstrates C++ threads, mutexes, condition-variable synchronization,
producer-consumer, FCFS, bounded resources, ACID commit/rollback, PostgreSQL
primary/foreign keys and unique constraints, row-level locking, and auditing.
PostgreSQL stays on the server computer; clients talk only to Crow.

If Crow or libpqxx is not already installed in UCRT64, install the matching
MSYS2 packages from the UCRT64 shell. Do not mix MinGW, MSVC, or vcpkg libraries
with this build:

```bash
pacman -S mingw-w64-ucrt-x86_64-crow mingw-w64-ucrt-x86_64-libpqxx mingw-w64-ucrt-x86_64-ninja
```

## Database

Find and start the installed PostgreSQL Windows service from elevated PowerShell
(the service name includes the installed PostgreSQL version):

```powershell
Get-Service -Name "postgresql*"
Start-Service -Name "postgresql-x64-16"
```

Replace `postgresql-x64-16` with the exact service name shown by `Get-Service`.
Then create a project-only database role and database (choose your own password):

```powershell
psql -U postgres -c "CREATE USER ticket_user WITH PASSWORD 'replace_this_password';"
psql -U postgres -c "CREATE DATABASE ticket_booking OWNER ticket_user;"
psql -U ticket_user -d ticket_booking -f database\schema.sql
psql -U ticket_user -d ticket_booking -f database\seed.sql
```

If re-running the role command reports that the role already exists, keep the
existing role and proceed. Use an appropriately protected local password; do not
commit `.env`.

## Configure, build and run

From the project directory:

```powershell
Copy-Item .env.example .env
notepad .env
```

Set `DATABASE_URL` in `.env` to the local database and credentials you created.
The checked-in example contains a placeholder password; replace it locally and
never commit `.env`. The backend requires `DATABASE_URL` in its process environment
and has no credential-bearing source-code fallback. Load the simple key/value file
into the current PowerShell process, configure and build with the UCRT64 compiler,
then add its runtime DLL directory to `PATH`:

```powershell
Get-Content .env | Where-Object { $_ -match '^[A-Za-z_][A-Za-z0-9_]*=' } | ForEach-Object {
  $name, $value = $_ -split '=', 2
  [Environment]::SetEnvironmentVariable($name, $value, 'Process')
}
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build-ucrt64 -G Ninja `
  -DCMAKE_CXX_COMPILER=C:\msys64\ucrt64\bin\g++.exe `
  -DCMAKE_PREFIX_PATH=C:\msys64\ucrt64 `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-ucrt64 --parallel
.\build-ucrt64\ticket_server.exe
```

Keep the current working directory at the project root because the server serves
`frontend` files using relative paths. The server binds to `0.0.0.0` by default,
port 18080. Open `http://127.0.0.1:18080`.

Useful settings in `.env`:

| Variable | Default | Description |
|---|---:|---|
| `SERVER_ADDRESS` | `0.0.0.0` | Listen on all interfaces for the LAN demo |
| `SERVER_PORT` | `18080` | Crow HTTP port |
| `DEMO_MODE` | `false` | Hide admin/diagnostic routes and bind to loopback when true |
| `DB_CONNECTION_LIMIT` | `8` | Maximum simultaneously leased DB connections |
| `WORKER_COUNT` | `4` | Fixed booking worker count |
| `REQUEST_QUEUE_CAPACITY` | `128` | Max waiting booking tasks; excess gets 503 |
| `SCHEDULING_POLICY` | `FCFS` | `FCFS`, `PRIORITY` or `AGING` |
| `AGING_INTERVAL_SECONDS` | `5` | Aging increment period |
| `RESERVATION_TTL_SECONDS` | `120` | Temporary reservation lifetime |
| `PAYMENT_FAILURE_RATE` | `0` | Random simulated failure probability when omitted |

Restart the server after changing environment variables.

## Temporary public demo with Cloudflare Quick Tunnel

Use an isolated demo database with non-sensitive data. Set `DEMO_MODE=true`;
demo mode forces Crow to `127.0.0.1` regardless of `SERVER_ADDRESS`, disables the
six admin/diagnostic endpoints with HTTP 404, and returns only `status` and
`demo_mode` from `/api/health`. PostgreSQL stays local; do not expose or tunnel
its port (normally 5432).

For the current PowerShell window, set demo mode and start Crow from the project
root:

```powershell
$env:DEMO_MODE = "true"
$env:SERVER_ADDRESS = "127.0.0.1"
.\build-ucrt64\ticket_server.exe
```

In a second PowerShell window, verify the restricted endpoints and normal event
and seat reads:

```powershell
python tests\demo_mode_test.py --base-url http://127.0.0.1:18080
```

This check creates a user and a successful booking to verify that booking still
works in demo mode. Use an available seat in a disposable demo database.

After installing `cloudflared`, start the temporary tunnel in that second window:

```powershell
cloudflared tunnel --url http://127.0.0.1:18080
```

Share only the generated HTTPS URL with demonstration participants. The UI and
API share that origin, so phones on other networks use the same URL automatically.
Stop the `cloudflared` process and backend when
finished; no port forwarding is required.

**Security limitation:** DEMO_MODE is not authentication. It hides the listed
diagnostic endpoints but leaves user creation, booking, reservation, cancellation,
and waiting-list APIs public. Anyone with the tunnel URL can attempt those actions.
Use a disposable database and data, never production credentials or real personal
information. A Quick Tunnel URL is public and does not provide audience access
control.

### Two-phone simultaneous-booking demonstration

1. Start PostgreSQL, apply the schema and seed data, and start Crow with
   `DEMO_MODE=true`.
2. Start the Quick Tunnel and open its generated HTTPS URL on two phones using
   different internet connections.
3. On each phone, create/select a different user, select the same event, and
   choose the same available seat.
4. Tap **Book selected seat** on both phones as close together as possible.
5. One request should confirm; the other should report the seat is already booked.
   The database row lock and unique active-booking index prevent a duplicate.
6. The booking remains in the demo database. Cancel it through the UI if desired,
   or use a fresh available seat for another run.

## Windows Firewall and LAN address

1. On Computer 1, open PowerShell and run `ipconfig`. Find the active Wi-Fi
   adapter's **IPv4 Address**, for example `192.168.1.20`. This is the server LAN
   IP; it may change after reconnecting to Wi-Fi.
2. Allow the Crow port through Windows Defender Firewall on Computer 1. In an
   elevated PowerShell window:

   ```powershell
   New-NetFirewallRule -DisplayName "Ticket Booking Demo 18080" -Direction Inbound `
     -Protocol TCP -LocalPort 18080 -Action Allow -Profile Private
   ```

   Change `18080` if `SERVER_PORT` differs. Use a private/trusted network profile
   and remove the rule after the demonstration if it is no longer needed.
3. Keep PostgreSQL's port (normally 5432) closed to client computers. The backend
   connects locally to PostgreSQL; browser traffic uses only Crow's port.
4. Connect Computer 2 to the same Wi-Fi and open
   `http://192.168.1.20:18080` (replace the sample IP/port). The page sends API
   requests to the same host and port automatically. If the page does not load,
   verify both computers are on the same non-guest network, the server process is
   running, and the IP/firewall rule are correct.

## Faculty live demo: same seat, same time

1. Start PostgreSQL and the backend on Computer 1; open the site on both computers.
2. Create different users on each browser (different email addresses).
3. Select the same event and seat A1 on both.
4. Click **Book selected seat** at nearly the same time.
5. One browser should receive “Booking confirmed.” The other should receive
   “Sorry, this seat is already booked.”
6. In Admin monitoring, run the consistency check; it should report zero duplicate
   active bookings. `/api/admin/seats/<seat_id>` should report
   `active_booking_count: 1`.

Automated repeat (A1 must be available):

```powershell
python -m pip install requests
python tests\concurrent_booking_test.py --base-url http://127.0.0.1:18080 --seat-label A1
```

The default test synchronizes 20 request threads. For exactly two synchronized users,
run:

```powershell
python tests\concurrent_booking_test.py --base-url http://127.0.0.1:18080 --seat-label A1 --users 2
```

The script reports successful/rejected requests and database verification. A
successful booking remains active; cancel it or use a fresh available seat before
repeating.

## Audit verification

With the backend running in normal mode and an available seeded seat, verify that
a failed simulated payment releases the seat and records a durable payment audit
event:

```powershell
python tests\booking_audit_test.py --base-url http://127.0.0.1:18080 --seat-label B1
```

The script creates a unique test user and writes a failed-payment record. Use a
disposable database or an available seat.

## Other tests

`tests\other_tests.py` tests simulated payment rollback, reservation expiry,
cancellation, automatic waiting-list offer/acceptance, and concurrent requests for
different seats. It expects seeded seats A2-A6 to be available. For a fast expiry
test, set `RESERVATION_TTL_SECONDS=3` in `.env`, restart, and run:

```powershell
$env:EXPIRY_WAIT_SECONDS = "5"
python tests\other_tests.py
```

The test modifies demo data. For a clean reset of a disposable database, drop and
recreate only the `ticket_booking` database, then apply schema and seed again.
