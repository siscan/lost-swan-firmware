// Presentation terminal (spec §15 phase 3.5).
//
// Entirely browser-side - the ESP32 pays nothing beyond the LittleFS bytes -
// and it sends the same §10.2a commands as every other surface.  Built for a
// kiosk: this is a candidate implementation of the terminal prop, a Pi in
// kiosk mode against lost.local, so nothing here assumes a phone viewport.
//
// Preferences live in localStorage, per browser.  They are presentation, not
// device state, and have no business in NVS.
"use strict";

const $ = (id) => document.getElementById(id);

const PREFS = {
  crt: false,     // off by default: costs readability, and the compositing is
                  // real work on a phone GPU
  click: true,
  dock: true,
};

function loadPrefs() {
  for (const k of Object.keys(PREFS)) {
    const v = localStorage.getItem("swan.term." + k);
    if (v !== null) PREFS[k] = v === "1";
  }
}

function savePref(k) {
  try {
    localStorage.setItem("swan.term." + k, PREFS[k] ? "1" : "0");
  } catch (_) {
    /* private mode: the toggle still works for this session */
  }
}

function applyPrefs() {
  $("screen").classList.toggle("crt", PREFS.crt);
  $("dock").classList.toggle("off", !PREFS.dock);
  document.querySelectorAll("[data-toggle]").forEach((b) => {
    b.classList.toggle("on", !!PREFS[b.dataset.toggle]);
  });
}

// --------------------------------------------------------------------------
// Key click, synthesized.  No samples to ship, and no decode cost: a short
// filtered noise burst with a fast decay is what a stiff key sounds like.
// The context is created on the first gesture because iOS refuses otherwise.
// --------------------------------------------------------------------------
let audio = null;

function ensureAudio() {
  if (audio) return audio;
  const Ctx = window.AudioContext || window.webkitAudioContext;
  if (!Ctx) return null;
  audio = new Ctx();
  return audio;
}

function clickSound(kind) {
  if (!PREFS.click) return;
  const ctx = ensureAudio();
  if (!ctx) return;
  if (ctx.state === "suspended") ctx.resume();

  const now = ctx.currentTime;
  const dur = kind === "go" ? 0.11 : 0.045;

  // Noise burst - the mechanical part of the click.
  const frames = Math.max(1, Math.floor(ctx.sampleRate * dur));
  const buf = ctx.createBuffer(1, frames, ctx.sampleRate);
  const data = buf.getChannelData(0);
  for (let i = 0; i < frames; i++) {
    const env = Math.pow(1 - i / frames, kind === "go" ? 3 : 7);
    data[i] = (Math.random() * 2 - 1) * env;
  }
  const noise = ctx.createBufferSource();
  noise.buffer = buf;

  const bp = ctx.createBiquadFilter();
  bp.type = "bandpass";
  bp.frequency.value = kind === "go" ? 900 : 1900;
  bp.Q.value = 0.9;

  const gain = ctx.createGain();
  gain.gain.value = kind === "go" ? 0.16 : 0.09;

  noise.connect(bp).connect(gain).connect(ctx.destination);
  noise.start(now);
  noise.stop(now + dur);

  // A short pitched thunk underneath gives it a body rather than a hiss.
  const osc = ctx.createOscillator();
  osc.type = "square";
  osc.frequency.setValueAtTime(kind === "go" ? 210 : 340, now);
  osc.frequency.exponentialRampToValueAtTime(kind === "go" ? 90 : 150, now + dur);
  const og = ctx.createGain();
  og.gain.setValueAtTime(kind === "go" ? 0.11 : 0.05, now);
  og.gain.exponentialRampToValueAtTime(0.0005, now + dur);
  osc.connect(og).connect(ctx.destination);
  osc.start(now);
  osc.stop(now + dur + 0.01);
}

// --------------------------------------------------------------------------
// The Numbers entry
// --------------------------------------------------------------------------
let entry = "";
const MAX_ENTRY = 24;

function renderEntry() {
  $("entry").textContent = entry;
}

function setMsg(text, cls) {
  const m = $("msg");
  m.textContent = text;
  m.className = "msg" + (cls ? " " + cls : "");
  clearTimeout(setMsg.t);
  if (text) setMsg.t = setTimeout(() => { m.textContent = ""; m.className = "msg"; }, 4000);
}

function press(label) {
  if (label === "DEL") {
    entry = entry.slice(0, -1);
  } else if (label === "CLR") {
    entry = "";
  } else if (label === "SP") {
    if (entry.length < MAX_ENTRY && entry.slice(-1) !== " ") entry += " ";
  } else if (label === "EXECUTE") {
    execute();
    return;
  } else if (entry.length < MAX_ENTRY) {
    // Digits do NOT auto-space - the Numbers include two-digit values, so the
    // pad cannot know where one ends.  The separator key is deliberate, and the
    // prompt says so, because "4815162342" parses as 481 5 16 23 42 and is
    // rejected with nothing on screen explaining why.  (The comment here used
    // to claim the opposite, which is how it went unnoticed.)
    entry += label;
  }
  renderEntry();
}

// CANCEL sits next to EXECUTE and ends a run that may have people watching it.
// A running countdown asks first; an idle one has nothing to lose.
function doCancel() {
  if (phase === "running" && !window.confirm(
        "Cancel the countdown? It is running, and this cannot be undone - " +
        "restarting means entering the Numbers again.")) {
    return;
  }
  bus.send("countdown.cancel");
  entry = "";
  renderEntry();
}

function execute() {
  if (!timeValid) {
    setMsg("CLOCK NOT SYNCED - THE DEADLINE CANNOT BE SET YET", "err");
    return;
  }
  const numbers = entry.trim().replace(/\s+/g, " ");
  if (!numbers) {
    setMsg("ENTER THE NUMBERS", "err");
    return;
  }
  bus.send("countdown.execute", numbers);
  pendingExecute = true;
}

const KEYS = [
  "1", "2", "3", "4", "5", "6",
  "7", "8", "9", "0", "SP", "DEL",
];

function buildPad() {
  const pad = $("pad");
  KEYS.forEach((k) => {
    const b = document.createElement("button");
    b.className = "key" + (k === "SP" || k === "DEL" ? " " : "");
    b.textContent = k === "SP" ? "␣" : k;
    b.dataset.key = k;
    pad.appendChild(b);
  });
  const clr = document.createElement("button");
  clr.className = "key wide";
  clr.textContent = "CLEAR";
  clr.dataset.key = "CLR";
  pad.appendChild(clr);

  const go = document.createElement("button");
  go.className = "key go";
  go.textContent = "EXECUTE";
  go.dataset.key = "EXECUTE";
  pad.appendChild(go);

  const cancel = document.createElement("button");
  cancel.className = "key wide";
  cancel.textContent = "CANCEL";
  cancel.dataset.key = "CANCEL";
  pad.appendChild(cancel);

  pad.addEventListener("pointerdown", (ev) => {
    const b = ev.target.closest(".key");
    if (!b) return;
    b.classList.add("down");
    clickSound(b.dataset.key === "EXECUTE" ? "go" : "key");
  });
  const up = (ev) => {
    const b = ev.target.closest ? ev.target.closest(".key") : null;
    document.querySelectorAll(".key.down").forEach((x) => x.classList.remove("down"));
    if (!b) return;
    if (b.dataset.key === "CANCEL") {
      doCancel();
      return;
    }
    press(b.dataset.key);
  };
  pad.addEventListener("pointerup", up);
  pad.addEventListener("pointercancel", () =>
    document.querySelectorAll(".key.down").forEach((x) => x.classList.remove("down")));
}

// A real keyboard drives the same path - a kiosk may well have one attached.
function bindKeyboard() {
  window.addEventListener("keydown", (ev) => {
    if (ev.metaKey || ev.ctrlKey || ev.altKey) return;
    // Not while a control has focus: preventDefault on Space or Enter there
    // suppresses the button's own activation, so tabbing to CRT and pressing
    // Space typed a separator instead of toggling it.
    const t = ev.target;
    if (t && (t.tagName === "BUTTON" || t.tagName === "INPUT" || t.tagName === "A" ||
              t.isContentEditable)) {
      return;
    }
    if (ev.repeat) return;   // a held digit used to fill the buffer
    let k = null;
    if (ev.key >= "0" && ev.key <= "9") k = ev.key;
    else if (ev.key === " ") k = "SP";
    else if (ev.key === "Backspace") k = "DEL";
    else if (ev.key === "Escape") k = "CLR";
    else if (ev.key === "Enter") k = "EXECUTE";
    else if (ev.key === "c" || ev.key === "C") k = "CANCEL";
    if (!k) return;
    ev.preventDefault();
    clickSound(k === "EXECUTE" ? "go" : "key");
    if (k === "CANCEL") { doCancel(); return; }
    press(k);
  });
}

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------
let flap = null;
let pendingExecute = false;

function mmss(total) {
  if (total < 0) total = 0;
  const m = Math.floor(total / 60);
  const s = total % 60;
  return String(m).padStart(3, "0") + ":" + String(s).padStart(2, "0");
}

// The deadline is absolute, so the readout counts down locally between state
// pushes rather than stepping once a second when a packet happens to arrive.
let target = 0;      // epoch seconds, 0 when not running
let skewMs = 0;      // device clock minus ours
let phase = "idle";
let secondsLive = 240;
let mode = "clock";        // what the display is actually doing
let clockFace = "--:--";   // the columns' own reading, for the idle face
let cueState = {};         // the audio block, for whether a cue could be heard
let timeValid = false;     // deadline commands are refused until SNTP has synced
let retryMax = 3;          // the device's REHOME_RETRIES, published on the wire

// Every reason a cue can fire and be inaudible.  All four are on the wire.
function audioSilent() {
  if (cueState.mute || cueState.volume === 0) return true;
  if (cueState.cues_present !== cueState.cues_total) return true;
  const qs = cueState.quiet_start_min, qe = cueState.quiet_end_min;
  if (qs === qe) return false;                       // quiet hours off
  const d = new Date(Date.now() + skewMs);
  const now = d.getHours() * 60 + d.getMinutes();
  return qs < qe ? (now >= qs && now < qe) : (now >= qs || now < qe);
}

function tickReadout() {
  const el = $("clock");
  if (!target || phase === "idle") {
    // 108:00 is the countdown's IDLE FACE, not a statement about the display.
    // On a prop CRT in a corridor, a clock-mode display showing 108:00 reads as
    // a stopped countdown - so say what is actually up there instead.
    if (mode && mode !== "countdown") {
      el.textContent = mode === "clock" ? clockFace : "— — : — —";
      el.className = "big dim";
      return;
    }
    el.textContent = "108:00";
    el.className = "big";
    return;
  }
  const now = (Date.now() + skewMs) / 1000;
  const rem = Math.max(0, Math.round(target - now));
  el.textContent = mmss(rem);
  el.className = "big" + (rem <= 60 ? " crit" : rem <= secondsLive ? " warn" : "");
}

function onState(s) {
  skewMs = s.t - Date.now();
  target = s.cd.target;
  phase = s.cd.phase;
  secondsLive = s.cd.seconds_live_s;
  $("mode").textContent = s.mode;
  mode = s.mode;
  cueState = s.audio || {};
  timeValid = !!s.time_valid;
  retryMax = (s.sys && s.sys.rehome_retries) || 3;
  // The columns' own reading, so the idle face shows what is on the wall
  // rather than a countdown that is not running.
  // The physical layout (spec 7.1): col 1 is AM/PM, cols 2-3 the hours, cols
  // 4-5 the minutes, with the band between them rendered as the colon.
  const f = s.cols.map((c) => (c.face === "blank" ? "" : c.face));
  // Only when the columns are actually showing a time.  Before SNTP syncs they
  // carry the WiFi glyph (spec 7.1), and rendering a glyph name into a clock
  // face reads as a fault - "wifi:" - rather than as "no time yet".
  const digits = [1, 2, 3, 4].every((i) => f[i] === "" || /^[0-9]$/.test(f[i]));
  const hhmm = (f[1] + f[2]) + ":" + (f[3] + f[4]);
  clockFace = digits && hhmm !== ":" ? (f[0] ? f[0] + " " : "") + hhmm : "— — : — —";

  const sub = {
    idle: "SYSTEM STANDBY",
    running: "ENTER THE NUMBERS TO RESET",
    zero: "SYSTEM FAILURE",
    spin: "SYSTEM FAILURE",
    reveal: "SYSTEM FAILURE",
  }[s.cd.phase] || s.cd.phase.toUpperCase();
  $("sub").textContent = s.mode === "countdown" ? sub : s.mode.toUpperCase() + " MODE";

  if (flap && !flap.primed) {
    flap.setAll(s.cols.map((c) => c.index));
    flap.primed = true;
  }
  if (flap) {
    flap.setStates(s.cols);
    // The state document is the authority; go events are lossy (see
    // flap.js reconcile()).  The dock is a mirror of a mirror and must not be
    // the one place the drift survives.
    flap.reconcile(s.cols, s.cfg && s.cfg.flaps_s_normal);
  }

  // A column that is not settled is a fact about the whole device, so it shows
  // here too - a kiosk never opens the Diagnostics page.
  // What the display IS comes before how it is doing.  A prop showing a
  // simulated countdown on a CRT in a corridor must say so on its own face.
  const m = s.motion || {};
  const rig = $("rig");
  const rigBits = [];
  if (m.maintenance) rigBits.push("MAINTENANCE");
  if (m.sim_columns > 0) rigBits.push("SIMULATED " + m.sim_columns + "/" + s.cols.length);
  if (m.disabled_columns > 0) rigBits.push(m.disabled_columns + " DISABLED");
  rig.style.display = rigBits.length ? "" : "none";
  rig.textContent = rigBits.join(" · ");

  const busy = s.cols
    .map((c, i) => ({ i, c }))
    .filter(({ c }) => c.mode !== "disabled" && (c.state !== "IDLE" || c.index < 0));
  const chip = $("motion");
  if (busy.length === 0 || m.maintenance) {
    chip.style.display = "none";
  } else {
    const faulted = busy.some(({ c }) => c.state === "FAULT");
    const retry = busy.find(({ c }) => c.retry > 0);
    chip.style.display = "";
    chip.className = "chip " + (faulted ? "bad" : "warn");
    const jammed = busy.some(({ c }) => c.cause === "jam");
    chip.textContent = (jammed ? "JAMMED " : faulted ? "FAULT " : "HOMING ") +
        busy.length + "/" + s.cols.length +
        (retry && !faulted ? " · try " + retry.c.retry + "/" + retryMax : "");
  }

  tickReadout();
}

// --------------------------------------------------------------------------
const bus = new SwanBus({
  onstatus: (up) => {
    const el = $("link");
    el.textContent = up ? "ONLINE" : "OFFLINE";
    el.className = "chip " + (up ? "ok" : "bad");
    // Re-prime on reconnect: snap to the display rather than animating a long
    // catch-up one flap at a time.  A kiosk may have been offline for hours.
    if (!up && flap) flap.primed = false;
  },
});

bus.on("state", onState)
   .on("go", (e) => flap && flap.flipTo(e.col, e.idx, e.flaps))
   .on("spin", (e) => flap && flap.spin(e.col, e.flaps, e.secs))
   .on("mode", (e) => { $("mode").textContent = e.name; })
   // "fired", not "played": mute, a volume of 0, quiet hours or a missing WAV
   // all produce silence, and every one of those facts is in the state document
   // this page already receives.
   .on("cue", (e) => setMsg((audioSilent() ? "⃠ " : "♪ ") + e.name.replace(/_/g, " "),
                            audioSilent() ? "warn" : "ok"))
   .on("result", (e) => {
     if (!e.res) return;
     if (e.res.ok) {
       if (pendingExecute) {
         setMsg("EXECUTE ACCEPTED", "ok");
         entry = "";
         renderEntry();
       }
     } else {
       setMsg(pendingExecute && e.res.err === "rejected"
                ? "INCORRECT — ENTER THE NUMBERS"
                : String(e.res.err || "REJECTED"),
              "err");
     }
     pendingExecute = false;
   });

function wireToggles() {
  document.querySelectorAll("[data-toggle]").forEach((b) => {
    b.onclick = () => {
      const k = b.dataset.toggle;
      PREFS[k] = !PREFS[k];
      savePref(k);
      applyPrefs();
      clickSound("key");
    };
  });
  $("full").onclick = () => {
    const el = document.documentElement;
    if (document.fullscreenElement) document.exitFullscreen();
    else if (el.requestFullscreen) el.requestFullscreen().catch(() => {});
  };
}

loadPrefs();
applyPrefs();
buildPad();
bindKeyboard();
wireToggles();
renderEntry();

SwanFlap.loadGlyphs("glyphs.svg").then((ok) => {
  if (ok && flap) flap.refresh();
});

fetch("/api/ring")
  .then((r) => r.json())
  .then((ring) => {
    flap = new SwanFlap.FlapDisplay($("display"), ring, { gapAfter: 2 });
  })
  .catch(() => { $("dock").classList.add("off"); })
  .then(() => {
    bus.connect();
    setInterval(tickReadout, 200);
  });
