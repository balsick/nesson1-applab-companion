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
  if (!slide) return;

  if (slide.title) {
    const h = document.createElement("h2");
    h.textContent = slide.title;
    into.appendChild(h);
  }

  // Consecutive bullets belong in one list; anything else is a paragraph.
  let list = null;
  for (const block of slide.blocks || []) {
    if (block.kind === "bullet") {
      if (!list) {
        list = document.createElement("ul");
        into.appendChild(list);
      }
      const li = document.createElement("li");
      li.innerHTML = emphasise(block.text);
      list.appendChild(li);
    } else {
      list = null;
      const p = document.createElement("p");
      p.innerHTML = emphasise(block.text);
      into.appendChild(p);
    }
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

  const next = deck[state.index + 1];
  $("upnext-title").textContent = next ? next.title || "(untitled slide)" : "— end of deck —";

  $("blanked").hidden = !state.blank;
  $("toggle-blank").classList.toggle("active", state.blank);
  $("toggle-pointer").classList.toggle("active", state.pointer);
  $("prev").disabled = state.index === 0;
  $("next").disabled = state.index >= state.count - 1;
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
$("toggle-blank").onclick = () => send("blank");
$("toggle-pointer").onclick = () => send("pointer");
$("timer").onclick = () => send("timer");
$("reset").onclick = () => send("reset");
$("fullscreen").onclick = toggleFullscreen;

function toggleFullscreen() {
  const stage = $("stage");
  if (document.fullscreenElement) document.exitFullscreen();
  else stage.requestFullscreen?.().catch(() => {});
}

document.addEventListener("keydown", (event) => {
  if (event.target.tagName === "INPUT") return;
  switch (event.key) {
    case "ArrowRight": case "PageDown": case " ": send("next"); break;
    case "ArrowLeft": case "PageUp": send("prev"); break;
    case "b": case "B": send("blank"); break;
    case "p": case "P": send("pointer"); break;
    case "f": case "F": toggleFullscreen(); break;
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

socket.on("connect", () => {
  fetch("api/state").then((r) => r.json()).then(applyLink).catch(() => {});
});

socket.on("link", applyLink);
socket.on("show", (payload) => {
  state = { ...state, ...payload };
  renderDeck();
});
socket.on("tick", renderTick);
