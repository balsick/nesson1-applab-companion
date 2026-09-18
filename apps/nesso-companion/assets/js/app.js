// Nesso N1 Companion - Phase 1 dashboard.
//
// The page is a mirror: every number here came from the Nesso over the
// companion WebSocket, was handed to a Python callback, and was pushed on from
// there. Nothing on this page talks to the Nesso directly.

const socket = io({ transports: ["websocket", "polling"] });

const $ = (id) => document.getElementById(id);

// Full-scale value for each bar, so the fill means something comparable.
const ACCEL_RANGE = 2.0; // g
const GYRO_RANGE = 250.0; // deg/s
const TILT_RANGE = 60.0; // degrees mapped to the edge of the circle

const AXES = ["X", "Y", "Z"];
const MAX_EVENTS = 60;

// --- Bars ------------------------------------------------------------------

function buildBars(container, prefix) {
  container.innerHTML = "";
  for (const axis of AXES) {
    const row = document.createElement("div");
    row.className = "bar-row";
    row.innerHTML =
      `<span>${axis.toLowerCase()}</span>` +
      `<div class="bar-track"><div class="bar-fill" id="${prefix}-${axis}-fill"></div></div>` +
      `<div class="val" id="${prefix}-${axis}-val">—</div>`;
    container.appendChild(row);
  }
}

// A signed value is drawn as a bar growing left or right from the centre.
function setBar(prefix, axis, value, range, decimals) {
  const fill = $(`${prefix}-${axis}-fill`);
  const val = $(`${prefix}-${axis}-val`);
  if (!fill || !val) return;
  if (value === undefined || value === null || Number.isNaN(value)) {
    fill.style.width = "0%";
    val.textContent = "—";
    return;
  }
  const ratio = Math.max(-1, Math.min(1, value / range));
  const half = Math.abs(ratio) * 50;
  fill.style.width = `${half}%`;
  fill.style.left = ratio >= 0 ? "50%" : `${50 - half}%`;
  val.textContent = value.toFixed(decimals);
}

buildBars($("accel-bars"), "accel");
buildBars($("gyro-bars"), "gyro");

// --- Link + handshake ------------------------------------------------------

function renderLink(state) {
  const connected = Boolean(state.connected);
  $("link-dot").classList.toggle("on", connected);
  $("link-url").textContent = state.url || "—";
  $("hs-app").textContent = state.app || "—";

  const device = state.device;
  if (connected && device) {
    $("link-text").textContent = `${device.name} connected`;
    $("hs-device").textContent = device.name || device.device || "Nesso N1";
    $("hs-fw").textContent = device.fw || "—";
    $("hs-address").textContent = device.address || "—";
  } else {
    $("link-text").textContent = "waiting for a Nesso";
    $("hs-device").textContent = "not connected";
    $("hs-fw").textContent = "—";
    $("hs-address").textContent = "—";
  }

  renderSubscriptions(state);
  renderWidgets(state.widgets || []);
  renderModulinoPresence(state.modulino_presence);
  renderDistance(state.modulino_live, state.modulino_presence);
  renderPixelsControls(state.pixels);
}

function renderSubscriptions(state) {
  const list = $("hs-subs");
  list.innerHTML = "";

  const add = (text, kind, note) => {
    const li = document.createElement("li");
    li.className = kind ? `kind-${kind}` : "";
    li.textContent = text;
    if (note) {
      const b = document.createElement("b");
      b.textContent = note;
      li.appendChild(b);
    }
    list.appendChild(li);
  };

  (state.sensors || []).forEach((name) => add(name, "sensor"));
  (state.buttons || []).forEach((name) => add(name, "button"));
  (state.modulinos || []).forEach((kind) => add(kind, "modulino", "modulino"));
  (state.widgets || []).forEach((w) => add(w.label || w.id, "widget", w.kind));

  if (!list.children.length) {
    const li = document.createElement("li");
    li.className = "empty";
    li.textContent = "nothing registered";
    list.appendChild(li);
  }
}

// --- Nesso screen mirror ---------------------------------------------------

function formatWidgetValue(value, kind) {
  // A button carries no state worth showing; the flash and the event log say
  // when it was tapped.
  if (kind === "button") return "\u2014";
  if (value === null || value === undefined) return "\u2014";
  if (typeof value === "boolean") return value ? "on" : "off";
  if (typeof value === "number") return Number.isInteger(value) ? String(value) : value.toFixed(2);
  return String(value);
}

function renderWidgets(widgets) {
  const host = $("widgets");
  host.innerHTML = "";
  if (!widgets.length) {
    host.innerHTML = '<p class="hint">This App registered no screen widgets.</p>';
    return;
  }
  for (const widget of widgets) {
    const el = document.createElement("div");
    el.className = "widget";
    el.id = `widget-${widget.id}`;

    el.dataset.kind = widget.kind;
    const value = formatWidgetValue(widget.value, widget.kind);
    const tone = widget.kind === "switch" ? (widget.value ? "on" : "off") : "";
    el.innerHTML =
      `<span class="label">${widget.label || widget.id}</span>` +
      `<span class="value ${tone}">${value}</span>` +
      `<span class="kind">${widget.kind}</span>`;
    host.appendChild(el);
  }
}

// Values change far more often than the widget set does, so they are patched in
// place instead of re-rendering the list.
function updateWidgetValues(values) {
  for (const [id, value] of Object.entries(values || {})) {
    const el = document.getElementById(`widget-${id}`);
    if (!el) continue;
    const span = el.querySelector(".value");
    if (!span) continue;
    const kind = el.dataset.kind;
    span.textContent = formatWidgetValue(value, kind);
    span.classList.toggle("on", kind === "switch" && value === true);
    span.classList.toggle("off", kind === "switch" && value === false);
  }
}

function flashWidget(id) {
  const el = $(`widget-${id}`);
  if (!el) return;
  el.classList.add("flash");
  setTimeout(() => el.classList.remove("flash"), 400);
}

// --- Modulinos -------------------------------------------------------------

// Matches PIXELS_MAX_MM in python/main.py, so the bar and the Modulino Pixels
// tell the same story.
const DISTANCE_MAX_MM = 400;

function renderModulinoPresence(presence) {
  const list = $("mod-presence");
  if (!list) return;
  list.innerHTML = "";
  const entries = Object.entries(presence || {});
  if (!entries.length) {
    list.innerHTML = '<li class="empty">this App subscribed to no Modulinos</li>';
    return;
  }
  for (const [kind, present] of entries) {
    const li = document.createElement("li");
    li.className = present ? "present" : "absent";
    li.textContent = kind;
    const note = document.createElement("b");
    note.textContent = present ? "attached" : "not attached";
    li.appendChild(note);
    list.appendChild(li);
  }
}

// --- Pixels controls -------------------------------------------------------
// The browser writes through the same handlers as the Nesso's own screen
// widgets, so whichever one you touch, both stay in step.

let suppressPixelsEcho = false;

function hueCss(hue, level) {
  // Mirrors hue_rgb() in main.py: level is HSV value, as a percentage.
  return `hsl(${hue}, 100%, ${Math.max(0, Math.min(100, level)) / 2}%)`;
}

function sendPixels(patch) {
  if (suppressPixelsEcho) return;
  socket.emit("pixels", patch);
}

function renderPixelsControls(state) {
  if (!state) return;
  const auto = $("px-auto");
  const hue = $("px-hue");
  const count = $("px-count");
  const level = $("px-level");
  const panel = document.querySelector(".pixels");
  if (!auto || !hue || !count || !level) return;

  // Echo guard: applying the server's state must not fire another emit.
  suppressPixelsEcho = true;
  auto.checked = Boolean(state.auto);
  hue.value = state.hue;
  count.value = state.count;
  level.value = state.level;
  suppressPixelsEcho = false;

  $("px-count-val").textContent = String(state.count);
  $("px-level-val").textContent = `${Math.round(state.level)}%`;
  $("px-swatch").style.background = hueCss(state.hue, state.level);
  if (panel) panel.dataset.auto = String(Boolean(state.auto));
  $("pixels-note").textContent = state.auto
    ? "following the distance sensor"
    : "writes straight to the LEDs on the Nesso";
}

function wirePixelsControls() {
  const auto = $("px-auto");
  const hue = $("px-hue");
  const count = $("px-count");
  const level = $("px-level");
  if (!auto || !hue || !count || !level) return;

  auto.addEventListener("change", () => sendPixels({ auto: auto.checked }));
  hue.addEventListener("input", () => {
    $("px-swatch").style.background = hueCss(hue.value, level.value);
    sendPixels({ hue: Number(hue.value) });
  });
  count.addEventListener("input", () => {
    $("px-count-val").textContent = count.value;
    sendPixels({ count: Number(count.value) });
  });
  // Brightness applies in auto mode too, so it never flips to manual.
  level.addEventListener("input", () => {
    $("px-level-val").textContent = `${level.value}%`;
    $("px-swatch").style.background = hueCss(hue.value, level.value);
    sendPixels({ level: Number(level.value) });
  });
}

wirePixelsControls();

function renderDistance(live, presence) {
  const value = $("distance-value");
  const fill = $("distance-fill");
  const note = $("distance-note");
  if (!value || !fill || !note) return;

  const mm = (live || {}).distance;
  const attached = (presence || {}).distance;

  if (!attached) {
    value.textContent = "\u2014";
    fill.style.width = "0%";
    note.textContent = "plug a Modulino Distance into the Qwiic connector";
    return;
  }
  if (mm === null || mm === undefined) {
    value.textContent = "\u2014";
    fill.style.width = "0%";
    note.textContent = "nothing in range";
    return;
  }
  // The bar grows as something gets closer, matching the Pixels bar.
  const nearness = 1 - Math.min(Math.max(mm, 0), DISTANCE_MAX_MM) / DISTANCE_MAX_MM;
  value.textContent = `${Math.round(mm)} mm`;
  fill.style.width = `${(nearness * 100).toFixed(1)}%`;
  fill.style.background = nearness > 0.66 ? "var(--red)" : nearness > 0.33 ? "var(--orange)" : "var(--accent)";
  note.textContent = "closer fills the bar, and lights the Modulino Pixels";
}

// --- Events ----------------------------------------------------------------

function timeOf(ts) {
  return new Date(ts * 1000).toLocaleTimeString([], { hour12: false });
}

function renderEvent(entry, atTop = true) {
  const list = $("events");
  const placeholder = list.querySelector(".placeholder");
  if (placeholder) placeholder.remove();

  const li = document.createElement("li");
  li.innerHTML =
    `<span class="src ${entry.source}">${entry.source}</span>` +
    `<span class="name">${entry.name}</span>` +
    `<span class="detail">${entry.detail}</span>` +
    `<span class="time">${timeOf(entry.ts)}</span>`;
  if (atTop) list.prepend(li);
  else list.appendChild(li);

  while (list.children.length > MAX_EVENTS) list.lastElementChild.remove();
  if (entry.source === "widget") flashWidget(entry.name);
}

function renderBacklog(events) {
  const list = $("events");
  list.innerHTML = "";
  if (!events.length) {
    list.innerHTML = '<li><span class="placeholder">no events yet</span></li>';
    return;
  }
  events.forEach((entry) => renderEvent(entry, false));
}

// --- Live values -----------------------------------------------------------

function renderValues(payload) {
  const v = payload.values || {};
  const has = payload.connected;

  for (const axis of AXES) {
    setBar("accel", axis, has ? v[`accelerometer${axis}`] : null, ACCEL_RANGE, 3);
    setBar("gyro", axis, has ? v[`gyroscope${axis}`] : null, GYRO_RANGE, 1);
  }

  const fmt = (value, digits, unit) =>
    has && value !== undefined && value !== null ? `${value.toFixed(digits)}${unit}` : "—";

  $("v-pitch").textContent = fmt(v.pitch, 1, "°");
  $("v-roll").textContent = fmt(v.roll, 1, "°");
  $("v-battery").textContent =
    has && v.batteryLevel !== undefined
      ? `${v.batteryLevel.toFixed(0)}% · ${(v.batteryVoltage ?? 0).toFixed(2)} V`
      : "—";

  updateWidgetValues(payload.widgets);
  renderModulinoPresence(payload.modulinos);
  renderDistance(payload.modulino_live, payload.modulinos);

  const dot = $("tilt-dot");
  const clamp = (value) => Math.max(-1, Math.min(1, (value || 0) / TILT_RANGE));
  dot.style.left = `${50 + clamp(has ? v.roll : 0) * 45}%`;
  dot.style.top = `${50 - clamp(has ? v.pitch : 0) * 45}%`;
}

// --- Socket ----------------------------------------------------------------

socket.on("connect", () => {
  fetch("api/state").then((r) => r.json()).then(renderLink).catch(() => {});
  fetch("api/events").then((r) => r.json()).then((d) => renderBacklog(d.events || [])).catch(() => {});
});

socket.on("disconnect", () => {
  $("link-dot").classList.remove("on");
  $("link-text").textContent = "web UI disconnected";
});

socket.on("link", renderLink);
socket.on("values", renderValues);
socket.on("event", (entry) => renderEvent(entry, true));
socket.on("backlog", (payload) => renderBacklog(payload.events || []));
socket.on("pixels", renderPixelsControls);
