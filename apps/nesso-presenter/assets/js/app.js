// Nesso Presenter - the projected surface.
//
// Every control here goes through the App, never straight to the DOM, so the
// browser and the Nesso's own screen can never disagree about which slide is up.

const socket = io({ transports: ["websocket", "polling"] });
const $ = (id) => document.getElementById(id);

let deck = [];
let state = { index: 0, count: 0, pointer: true, blank: false, running: false };

// A remote that has said nothing for this long is treated as gone, so a flat
// battery reading is not mistaken for a live one.
const STALE_MS = 4000;
let lastTickAt = 0;

// --- Slides ----------------------------------------------------------------

function renderSlide(slide, into) {
  into.innerHTML = "";
  into.classList.remove("has-image", "title-only");
  if (!slide) return;

  // An illustration splits the slide in two; without one the words get the
  // whole stage and are centred.
  if (slide.image) into.classList.add("has-image");
  if (!(slide.blocks || []).length) into.classList.add("title-only");

  const words = document.createElement("div");
  words.className = "words";

  if (slide.title) {
    const h = document.createElement("h2");
    h.textContent = slide.title;
    words.appendChild(h);
  }

  // Consecutive bullets belong in one list; anything else stands alone.
  let list = null;
  for (const block of slide.blocks || []) {
    if (block.kind === "bullet") {
      if (!list) {
        list = document.createElement("ul");
        words.appendChild(list);
      }
      const li = document.createElement("li");
      li.innerHTML = emphasise(block.text);
      list.appendChild(li);
    } else if (block.kind === "code") {
      list = null;
      const pre = document.createElement("pre");
      pre.className = "code";
      pre.textContent = block.text;
      words.appendChild(pre);
    } else {
      list = null;
      const p = document.createElement("p");
      p.className = block.kind === "lead" ? "lead" : "";
      p.innerHTML = emphasise(block.text);
      words.appendChild(p);
    }
  }
  into.appendChild(words);

  // A live panel takes the figure half and is filled in by each tick.
  if (slide.live) {
    into.classList.add("has-image");
    const panel = document.createElement("div");
    panel.className = "figure live";
    panel.id = `live-${slide.live}`;
    panel.innerHTML =
      slide.live === "pixels" ? PIXELS_PANEL : slide.live === "credits" ? CREDITS_PANEL : "";
    into.appendChild(panel);
  }

  if (slide.image) {
    const figure = document.createElement("div");
    figure.className = "figure";
    const img = document.createElement("img");
    img.src = slide.image.src;
    img.alt = slide.image.alt || "";
    figure.appendChild(img);
    into.appendChild(figure);
  }
}

// The deck is plain text, so only **bold** is honoured - and it is escaped
// first, because the deck is a file anyone can edit.
function emphasise(text) {
  const escaped = text
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
  return escaped.replace(/\*\*(.+?)\*\*/g, "<strong>$1</strong>");
}

function renderDeck() {
  renderSlide(deck[state.index], $("slide"));
  const label = `${state.index + 1} / ${state.count}`;
  $("counter").textContent = label;
  $("stage-badge").textContent = label;
  const progress = state.count > 1 ? (state.index / (state.count - 1)) * 100 : 100;
  $("progress").style.width = `${progress}%`;

  const next = deck[state.index + 1];
  $("upnext-title").textContent = next ? next.title || "(untitled slide)" : "— end of deck —";

  $("toggle-pointer").classList.toggle("active", state.pointer);
  $("prev").disabled = state.index === 0;
  $("next").disabled = state.index >= state.count - 1;
}

// --- Live panels -----------------------------------------------------------

const PIXELS_PANEL = `
  <div class="strip" id="strip"></div>
  <p class="live-note" id="pixels-note">waiting for a Modulino Pixels…</p>
  <p class="live-sub" id="pixels-sub"></p>`;

const CREDITS_PANEL = `
  <dl class="credits" id="credits"></dl>`;

function renderLive(live, credits) {
  const strip = $("strip");
  if (strip) {
    // Eight dots mirroring the colours actually on the module.
    if (strip.children.length !== 8) {
      strip.innerHTML = "";
      for (let i = 0; i < 8; i++) strip.appendChild(document.createElement("span"));
    }
    const bar = live.pixels || [];
    [...strip.children].forEach((led, i) => {
      const [r, g, b] = bar[i] || [0, 0, 0];
      const lit = r + g + b > 12;
      led.style.background = `rgb(${r},${g},${b})`;
      // Scale the glow with the colour so a dark LED does not appear to shine.
      led.style.boxShadow = lit ? `0 0 16px 5px rgba(${r},${g},${b},.55)` : "none";
      led.classList.toggle("off", !lit);
    });

    const attached = (live.attached || {}).pixels;
    $("pixels-note").textContent = attached
      ? live.driving
        ? "attached, and this slide is driving it"
        : "attached"
      : "waiting for a Modulino Pixels…";
    $("pixels-note").classList.toggle("ok", Boolean(attached));
    $("pixels-sub").textContent =
      live.distance != null
        ? `a Distance module is steering it — ${Math.round(live.distance)} mm`
        : attached
        ? "plug a Distance module in too and it takes over"
        : "it will be picked up within three seconds";
  }

  const list = $("credits");
  if (list && credits) {
    const rows = [
      ["remote", credits.device ? `${credits.device} · firmware ${credits.firmware}` : "not connected"],
      ["values it streams", credits.sensors.join(", ") || "none"],
      ["buttons claimed", credits.buttons.join(", ") || "none"],
      ["widgets it draws", `${credits.widgets.length} — ${credits.widgets.join(", ")}`],
      ["Modulinos", Object.entries(credits.modulinos).map(([k, v]) => `${k} ${v ? "✓" : "✗"}`).join("  ") || "none asked for"],
      ["slides", String(credits.slides)],
      ["advanced from the remote", String(credits.from_remote)],
      ["advanced from the browser", String(credits.from_browser)],
    ];
    list.innerHTML = rows
      .map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`)
      .join("");
  }
}

// --- Live readouts ---------------------------------------------------------

function renderTick(tick) {
  lastTickAt = Date.now();

  $("link-dot").classList.toggle("on", tick.connected);
  $("link-name").textContent = tick.connected ? "remote connected" : "no remote";
  $("link-note").textContent = tick.connected
    ? "tilt to point, KEY1 to advance"
    : "the deck still works from this browser";

  const battery = tick.battery || {};
  const readout = $("battery-readout");
  if (tick.connected && battery.volts != null) {
    // 3.3 V empty to 4.2 V full is the usable span of this cell.
    const pct = Math.max(0, Math.min(1, (battery.volts - 3.3) / 0.9)) * 100;
    $("battery-fill").style.width = `${pct.toFixed(0)}%`;
    $("battery-volts").textContent = `${battery.volts.toFixed(2)} V`;
    $("battery-note").textContent = battery.low
      ? "remote battery low"
      : `remote battery${battery.level != null ? ` · ${Math.round(battery.level)}%` : ""}`;
    readout.classList.toggle("low", Boolean(battery.low));
  } else {
    $("battery-fill").style.width = "0%";
    $("battery-volts").textContent = "—";
    $("battery-note").textContent = "remote battery";
    readout.classList.remove("low");
  }

  const pointer = tick.pointer || {};
  const dot = $("pointer");
  dot.hidden = !pointer.on;
  if (pointer.on) {
    dot.style.left = `${(pointer.x * 100).toFixed(2)}%`;
    dot.style.top = `${(pointer.y * 100).toFixed(2)}%`;
  }

  if (tick.live) renderLive(tick.live, tick.credits);

  $("clock").textContent = tick.clock;
  $("clock").classList.toggle("running", Boolean(tick.running));
  $("clock-note").textContent = tick.running ? "running" : "stopped";
  $("timer").textContent = tick.running ? "Pause" : "Start";
}

// A tick that stops arriving means the App went away, not that the battery hit
// zero; dim the readouts rather than leave a stale number looking live.
setInterval(() => {
  const stale = Date.now() - lastTickAt > STALE_MS;
  $("battery-readout").classList.toggle("stale", stale);
  $("remote-readout").classList.toggle("stale", stale);
}, 1000);

// --- Controls --------------------------------------------------------------

const send = (action, extra = {}) => socket.emit("show", { action, ...extra });

$("prev").onclick = () => send("prev");
$("next").onclick = () => send("next");
$("toggle-pointer").onclick = () => send("pointer");
$("timer").onclick = () => send("timer");
$("reset").onclick = () => send("reset");
$("fullscreen").onclick = toggleFullscreen;
$("toggle-present").onclick = togglePresent;

// Real fullscreen needs a user gesture in the page, so the remote cannot ask for
// it. Present mode is the part that does not: it is only CSS, so it always works,
// and it gets the deck to fill everything below the status bar.
function setPresent(on) {
  document.body.classList.toggle("present", on);
  $("toggle-present").classList.toggle("active", on);
  $("toggle-present").textContent = on ? "Exit present" : "Present";
}

function togglePresent() {
  setPresent(!document.body.classList.contains("present"));
}

// The button and the f key are real gestures, so they can have the real thing —
// and present mode with it, since fullscreen with the controls still showing
// would be odd.
function toggleFullscreen() {
  if (document.fullscreenElement) {
    document.exitFullscreen();
    return;
  }
  setPresent(true);
  $("stage").requestFullscreen?.().catch(() => {});
}

// Leaving fullscreen by Escape should not strand the deck in present mode.
document.addEventListener("fullscreenchange", () => {
  if (!document.fullscreenElement) setPresent(false);
});

let toastTimer = null;
function toast(text, ms = 3200) {
  const el = $("toast");
  if (!el) return;
  el.textContent = text;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(hideToast, ms);
}
function hideToast() {
  clearTimeout(toastTimer);
  const el = $("toast");
  if (el) el.hidden = true;
}

document.addEventListener("keydown", (event) => {
  if (event.target.tagName === "INPUT") return;
  switch (event.key) {
    case "ArrowRight": case "PageDown": case " ": send("next"); break;
    case "ArrowLeft": case "PageUp": send("prev"); break;
    case "b": case "B": case "f": case "F": toggleFullscreen(); break;
    case "p": case "P": send("pointer"); break;
    case "r": case "R": send("recentre"); break;
    default: return;
  }
  event.preventDefault();
});

// --- Socket ----------------------------------------------------------------

function applyLink(link) {
  if (link.deck) deck = link.deck;
  if (link.show) {
    state = { ...state, ...link.show };
    renderDeck();
  }
}

// Opening the deck at .../#present starts it filled, which is what you want for
// a bookmark pointed at the projector.
if (location.hash === "#present") setPresent(true);

socket.on("connect", () => {
  fetch("api/state").then((r) => r.json()).then(applyLink).catch(() => {});
});

socket.on("link", applyLink);
socket.on("show", (payload) => {
  state = { ...state, ...payload };
  renderDeck();
});
socket.on("tick", renderTick);
// The remote asks for present mode, which never gets refused.
socket.on("fullscreen", () => togglePresent());
// KEY2 recentres. Say so: if the pointer was already near the middle, nothing
// visible moves, and a control that looks dead gets pressed again.
socket.on("recentred", (payload) => toast(`Pointer recentred · ${payload.source}`, 1600));
