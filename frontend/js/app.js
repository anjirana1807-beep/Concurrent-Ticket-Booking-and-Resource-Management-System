(() => {
  "use strict";

  const $ = (selector) => document.querySelector(selector);
  const state = {
    user: JSON.parse(localStorage.getItem("ticket-user") || "null"),
    eventId: null,
    seat: null,
    reservationId: null,
    demoMode: false
  };

  const api = async (path, options = {}) => {
    const response = await fetch(path, {
      ...options,
      headers: { "Content-Type": "application/json", ...(options.headers || {}) }
    });
    const contentType = response.headers.get("content-type") || "";
    if (!contentType.includes("application/json")) {
      throw new Error("The API did not return JSON. Check that the Crow backend is running.");
    }
    const payload = await response.json();
    if (!response.ok) {
      const error = new Error(payload.message || `Request failed (${response.status})`);
      error.status = response.status;
      throw error;
    }
    return payload;
  };
  let toastTimer;
  function notify(message, error = false) {
    const toast = $("#toast");
    toast.textContent = message;
    toast.classList.toggle("error", error);
    toast.classList.add("show");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => toast.classList.remove("show"), 4200);
  }
  function setStatus(message) {
    $("#api-status").textContent = message;
  }
  function requireUser() {
    if (!state.user) {
      notify("Create or select your user before booking.", true);
      return false;
    }
    return true;
  }
  async function loadEvents() {
    const data = await api("/api/events");
    const select = $("#events");
    select.replaceChildren();
    data.events.forEach((event) => {
      const option = document.createElement("option");
      option.value = event.id;
      option.textContent = event.name;
      option.dataset.description = `${event.venue} · ${new Date(event.starts_at).toLocaleString()}`;
      select.append(option);
    });
    state.eventId = data.events.length ? Number(select.value) : null;
    $("#event-details").textContent = select.selectedOptions[0]?.dataset.description || "No events have been seeded.";
    await loadSeats();
  }
  async function loadSeats() {
    const grid = $("#seat-grid");
    grid.replaceChildren();
    state.seat = null;
    updateActions();
    if (!state.eventId) return;
    const { seats } = await api(`/api/events/${state.eventId}/seats`);
    seats.forEach((seat) => {
      const button = document.createElement("button");
      const statusClass = seat.status.toLowerCase();
      button.className = `seat ${statusClass}`;
      button.innerHTML = `${seat.label}<small>${seat.status}</small>`;
      button.setAttribute("aria-label", `${seat.label}: ${seat.status}`);
      button.addEventListener("click", () => {
        state.seat = seat;
        grid.querySelectorAll(".seat").forEach((item) => item.classList.remove("selected"));
        button.classList.add("selected");
        $("#selection").textContent = seat.status === "AVAILABLE"
          ? `Selected ${seat.label}.`
          : `${seat.label} is ${seat.status.toLowerCase()}; you may join its waiting list.`;
        updateActions();
      });
      grid.append(button);
    });
    $("#selection").textContent = "Select an available seat to continue.";
  }
  function updateActions() {
    const canAct = Boolean(state.seat && state.user && state.seat.status === "AVAILABLE");
    $("#book-seat").disabled = !canAct;
    $("#reserve-seat").disabled = !canAct;
    $("#wait-seat").disabled = !state.seat || !state.user || state.seat.status === "AVAILABLE";
  }
  async function refreshUserBookings() {
    if (!state.user) return;
    const [{ bookings }, { reservations }] = await Promise.all([
      api(`/api/users/${state.user.id}/bookings`),
      api(`/api/users/${state.user.id}/reservations`)
    ]);
    renderReservationOffers(reservations);
    if (!bookings.length) {
      $("#bookings-list").textContent = "No bookings yet.";
      $("#bookings-list").classList.add("muted");
      return;
    }
    const table = document.createElement("table");
    table.innerHTML = "<thead><tr><th>Event / seat</th><th>Status</th><th>Booking ID</th><th></th></tr></thead>";
    const body = document.createElement("tbody");
    bookings.forEach((booking) => {
      const row = document.createElement("tr");
      const title = document.createElement("td");
      title.textContent = `${booking.event_name} · ${booking.seat_label}`;
      const status = document.createElement("td");
      status.textContent = booking.status;
      const id = document.createElement("td");
      id.textContent = booking.id;
      const action = document.createElement("td");
      if (booking.status === "CONFIRMED") {
        const cancel = document.createElement("button");
        cancel.className = "button danger";
        cancel.textContent = "Cancel";
        cancel.addEventListener("click", async () => {
          try {
            const result = await api(`/api/bookings/${booking.id}/cancel`, {
              method: "POST", body: JSON.stringify({ user_id: state.user.id })
            });
            notify(result.message);
            await Promise.all([refreshUserBookings(), loadSeats(), refreshAdmin()]);
          } catch (error) { notify(error.message, true); }
        });
        action.append(cancel);
      }
      row.append(title, status, id, action);
      body.append(row);
    });
    table.append(body);
    $("#bookings-list").replaceChildren(table);
    $("#bookings-list").classList.remove("muted");
  }
  function renderReservationOffers(reservations) {
    const container = $("#reservation-offers");
    container.replaceChildren();
    reservations.forEach((reservation) => {
      const box = document.createElement("div");
      box.className = "payment-box";
      const text = document.createElement("span");
      text.textContent = `Waiting-list offer for ${reservation.seat_label} · expires ${new Date(reservation.expires_at).toLocaleTimeString()}`;
      const pay = document.createElement("button");
      pay.className = "button";
      pay.textContent = "Accept offer (simulate payment)";
      pay.addEventListener("click", async () => {
        try {
          const result = await api(`/api/reservations/${reservation.id}/pay`, {
            method: "POST", body: JSON.stringify({ payment_success: true })
          });
          notify(result.message, !result.success);
          await Promise.all([loadSeats(), refreshUserBookings(), refreshAdmin()]);
        } catch (error) {
          notify(error.message, true);
          if (error.status === 409) {
            await Promise.all([loadSeats(), refreshUserBookings(), refreshAdmin()]);
          }
        }
      });
      box.append(text, pay);
      container.append(box);
    });
  }
  async function refreshAdmin() {
    if (state.demoMode) return;
    const stats = await api("/api/admin/stats");
    const labels = [
      ["total_events", "Events"], ["total_seats", "Total seats"],
      ["available_seats", "Available"], ["reserved_seats", "Reserved"],
      ["booked_seats", "Booked"], ["successful_bookings", "Successful"],
      ["failed_bookings", "Failed"], ["cancelled_bookings", "Cancelled"],
      ["waiting_requests", "Waiting list"], ["active_worker_threads", "Active workers"],
      ["request_queue_size", "Queue size"],
      ["request_queue_peak_size", "Queue peak"],
      ["request_queue_capacity", "Queue capacity"],
      ["database_connections_in_use", "DB connections in use"],
      ["database_connection_limit", "DB connection limit"],
      ["database_connections_peak", "DB connections peak"]
    ];
    const grid = $("#admin-stats");
    grid.replaceChildren();
    labels.forEach(([key, title]) => {
      const card = document.createElement("div");
      card.className = "metric";
      const value = document.createElement("strong");
      value.textContent = stats[key];
      const label = document.createElement("span");
      label.textContent = title;
      card.append(value, label);
      grid.append(card);
    });
    setStatus(`Backend online · ${stats.scheduling_policy}`);
  }

  if (state.user) {
    $("#user-label").textContent = `${state.user.name} · ${state.user.email}`;
    $("#user-name").value = state.user.name;
    $("#user-email").value = state.user.email;
  }
  updateActions();

  $("#user-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    try {
      const user = await api("/api/users", {
        method: "POST",
        body: JSON.stringify({ name: $("#user-name").value.trim(), email: $("#user-email").value.trim() })
      });
      state.user = { id: user.user_id, name: $("#user-name").value.trim(), email: $("#user-email").value.trim() };
      localStorage.setItem("ticket-user", JSON.stringify(state.user));
      $("#user-label").textContent = `${state.user.name} · ${state.user.email}`;
      updateActions();
      notify("User ready on this device.");
      await refreshUserBookings();
    } catch (error) { notify(error.message, true); }
  });
  $("#events").addEventListener("change", async (event) => {
    state.eventId = Number(event.target.value);
    $("#event-details").textContent = event.target.selectedOptions[0]?.dataset.description || "";
    try { await loadSeats(); } catch (error) { notify(error.message, true); }
  });
  $("#refresh-events").addEventListener("click", async () => {
    try { await loadEvents(); } catch (error) { notify(error.message, true); }
  });
  $("#book-seat").addEventListener("click", async () => {
    if (!requireUser()) return;
    try {
      const result = await api("/api/bookings", {
        method: "POST",
        body: JSON.stringify({ seat_id: state.seat.id, user_id: state.user.id, payment_success: true })
      });
      notify(result.message);
      await Promise.all([loadSeats(), refreshUserBookings(), refreshAdmin()]);
    } catch (error) {
      notify(error.message, true);
      await loadSeats();
    }
  });
  $("#reserve-seat").addEventListener("click", async () => {
    if (!requireUser()) return;
    try {
      const result = await api("/api/reservations", {
        method: "POST",
        body: JSON.stringify({ seat_id: state.seat.id, user_id: state.user.id })
      });
      state.reservationId = result.reservation_id;
      $("#reservation-label").textContent = `Reservation ${state.reservationId} · ${result.message}`;
      $("#reservation-payment").hidden = false;
      notify(result.message);
      await loadSeats();
    } catch (error) { notify(error.message, true); }
  });
  async function pay(success) {
    try {
      const result = await api(`/api/reservations/${state.reservationId}/pay`, {
        method: "POST", body: JSON.stringify({ payment_success: success })
      });
      $("#reservation-payment").hidden = true;
      state.reservationId = null;
      notify(result.message, !result.success);
      await Promise.all([loadSeats(), refreshUserBookings(), refreshAdmin()]);
    } catch (error) {
      notify(error.message, true);
      if (error.status === 409) {
        $("#reservation-payment").hidden = true;
        state.reservationId = null;
        await Promise.all([loadSeats(), refreshUserBookings(), refreshAdmin()]);
      }
    }
  }
  $("#pay-success").addEventListener("click", () => pay(true));
  $("#pay-failure").addEventListener("click", () => pay(false));
  $("#wait-seat").addEventListener("click", async () => {
    if (!requireUser()) return;
    try {
      const result = await api("/api/waiting-list", {
        method: "POST",
        body: JSON.stringify({ seat_id: state.seat.id, user_id: state.user.id })
      });
      notify(result.message, !result.success);
      await refreshAdmin();
    } catch (error) { notify(error.message, true); }
  });
  $("#refresh-bookings").addEventListener("click", () => refreshUserBookings().catch((error) => notify(error.message, true)));
  $("#refresh-admin").addEventListener("click", () => refreshAdmin().catch((error) => notify(error.message, true)));
  $("#check-consistency").addEventListener("click", async () => {
    try { $("#admin-output").textContent = JSON.stringify(await api("/api/admin/consistency"), null, 2); }
    catch (error) { notify(error.message, true); }
  });
  $("#show-scheduling").addEventListener("click", async () => {
    try { $("#admin-output").textContent = JSON.stringify(await api("/api/scheduling/demo"), null, 2); }
    catch (error) { notify(error.message, true); }
  });
  $("#show-deadlock").addEventListener("click", async () => {
    try { $("#admin-output").textContent = JSON.stringify(await api("/api/deadlock/demo"), null, 2); }
    catch (error) { notify(error.message, true); }
  });
  $("#show-audit").addEventListener("click", async () => {
    try { $("#admin-output").textContent = JSON.stringify(await api("/api/admin/audit"), null, 2); }
    catch (error) { notify(error.message, true); }
  });

  async function initialize() {
    try {
      const health = await api("/api/health");
      state.demoMode = health.demo_mode === true;
      $("#admin-panel").hidden = state.demoMode;
      await Promise.all([loadEvents(), refreshAdmin()]);
      if (state.user) await refreshUserBookings();
      setStatus("Backend online");
    } catch (error) {
      setStatus("Backend unavailable");
      notify(error.message, true);
    }
  }
  initialize();
})();
