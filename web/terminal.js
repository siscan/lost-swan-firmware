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

  // CONTENT, as opposed to the presentation toggles above (rule 1): these say
  // WHAT is on screen, those say how it looks, and the two compose freely.
  // Per browser, in localStorage - presentation is not device state and has no
  // business in NVS.
  protocol: false,   // the purist station screen: near-black, inert until 4:00
  egg: false,        // the chat easter egg, on Swan
};

// The station is content too, but it is a name rather than a flag.
const STATION_KEY = "swan.term.station";
let stationName = "swan";

// `boot` is NO LONGER A TOGGLE.  It shipped default-off, and that was the whole
// of "the boot animation never plays on a clean load": playBoot() returned
// immediately because localStorage was empty on a fresh profile, so the
// incognito reproduction guaranteed the failure rather than ruling anything
// out.  It now plays on every load of this page while the station is SWAN
// (defect 1, 2026-08-25), always skippable, and can be replayed deliberately
// from the strip or by typing LOGO at the Swan prompt.

function loadPrefs() {
  // The WHOLE body is guarded, not just the station read.  A browser with
  // storage blocked outright throws on the property access itself, and an
  // exception escaping here stops the module executing - which is a blank page
  // on a healthy board, the single worst-diagnosing failure this UI has.
  try {
    for (const k of Object.keys(PREFS)) {
      const v = localStorage.getItem("swan.term." + k);
      if (v !== null) PREFS[k] = v === "1";
    }
    const st = localStorage.getItem(STATION_KEY);
    if (st === "swan" || st === "pearl" || st === "flame") stationName = st;
  } catch (_) {
    /* private mode, or storage disabled: the defaults are a working terminal */
  }
}

// Rule 1: setting the station touches NOTHING else.  It is content; the
// presentation toggles keep whatever they were.
function setStation(name) {
  if (name !== "swan" && name !== "pearl" && name !== "flame") return;
  if (name === stationName) return;
  stationName = name;
  try { localStorage.setItem(STATION_KEY, name); } catch (_) { /* private mode */ }
  applyPrefs();
  SwanTerm.emit("station", name);
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
  document.querySelectorAll("[data-station]").forEach((b) => {
    b.classList.toggle("on", b.dataset.station === stationName);
  });
  // The dock is presentation and MUST survive the content choice: with MIRROR
  // on it stays visible in protocol mode too, because it is the feedback the
  // physical flaps give in the room.
  document.documentElement.classList.toggle("mirror-on", !!PREFS.dock);
  // The phase 7 modules own their own DOM, so they have to be told.  Each is
  // optional: a page served without protocol.js must still toggle CRT.
  if (window.SwanProtocol) window.SwanProtocol.apply();
  if (window.SwanTerm) SwanTerm.emit("prefs", PREFS);
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
let tzOffsetS = 0;   // the DEVICE's UTC offset, not this browser's
let phase = "idle";
let secondsLive = 240;
let secondsMode = "seconds";   // cd.seconds_mode, for the rendering contract
let mode = "clock";        // what the display is actually doing
let cueState = {};         // the audio block, for whether a cue could be heard
let timeValid = false;     // deadline commands are refused until SNTP has synced
let retryMax = 3;          // the device's REHOME_RETRIES, published on the wire
let h24 = false;

// The device's wall clock as a Date whose UTC fields ARE its local fields -
// the only way to render another machine's zone without shipping a tz database
// to the browser.  tzOffsetS comes from the state document.
function deviceNow() {
  return new Date(Date.now() + skewMs + tzOffsetS * 1000);
}

// The device's own clock, skew-corrected, to the minute.  Deliberately NOT the
// columns' reading: the drums are floored to clock.granularity_min to save
// wear, and a screen has nothing to wear out.
function realClockFace() {
  // The DEVICE's zone, off the wire.  getHours() would use the browser's, and
  // a kiosk Pi is UTC out of the box - so the CRT read eight hours from the
  // drums beside it, with both of them correct about the instant.
  const d = deviceNow();
  const h = d.getUTCHours();
  const mm = String(d.getUTCMinutes()).padStart(2, "0");
  if (h24) return String(h).padStart(2, "0") + ":" + mm;
  const h12 = h % 12 === 0 ? 12 : h % 12;
  return (h < 12 ? "AM " : "PM ") + h12 + ":" + mm;
}

// Every reason a cue can fire and be inaudible.  All four are on the wire.
function audioSilent() {
  if (cueState.mute || cueState.volume === 0) return true;
  if (cueState.cues_present !== cueState.cues_total) return true;
  const qs = cueState.quiet_start_min, qe = cueState.quiet_end_min;
  if (qs === qe) return false;                       // quiet hours off
  // Quiet hours are the DEVICE's hours - the firmware evaluates them in its own
  // zone, so a viewer in another one must not draw a different conclusion.
  const d = deviceNow();
  const now = d.getUTCHours() * 60 + d.getUTCMinutes();
  return qs < qe ? (now >= qs && now < qe) : (now >= qs || now < qe);
}

// THE RENDERING CONTRACT, spec 7.3: displayed = ceil(remaining / step) * step.
//
// The same formula the firmware uses, deliberately - this readout and the
// separate terminal prop both derive their display from the same deadline, and
// two screens showing one countdown must not disagree by a whole step.  It is a
// port of components/modes/render.cpp's countdown_step_s / countdown_shown_s;
// if you change one, change both.
//
// CEILING, not floor: the show holds 108:00 until a full minute has elapsed, so
// a value owns the window ABOVE it.  That also puts 000:00 exactly on the
// deadline instead of a second before its own klaxon.
function countdownStepS(secondsMode, remainingS, liveS) {
  if (secondsMode === "minutes") return 60;
  if (remainingS > Math.floor(liveS / 60) * 60) return 60;
  return secondsMode === "seconds" ? 1 : 10;
}

function countdownShownS(secondsMode, remainingS, liveS) {
  if (!(remainingS > 0)) return 0;
  const step = countdownStepS(secondsMode, remainingS, liveS);
  return Math.ceil(remainingS / step) * step;
}

// THE READOUT FOLLOWS THE MODE (defect 2, 2026-08-25).
//
// It used to follow the COUNTDOWN's state, so a display in clock mode - badge
// reading CLOCK, flaps correctly showing PM 11:15 - put `101:00` on the CRT
// under a "CLOCK MODE" caption, because a countdown was running in the
// background. A countdown number captioned as a clock is worse than either
// number alone.
//
// A run happening behind another mode is surfaced instead as its own chip, so
// running-but-hidden is visible somewhere at all times.
function tickReadout() {
  const el = $("clock");
  updateCountdownChip();

  if (mode === "clock") {
    el.textContent = timeValid ? realClockFace() : "— — : — —";
    el.className = "big dim";
    return;
  }
  if (mode === "message") {
    el.textContent = "— — : — —";
    el.className = "big dim";
    return;
  }
  // Countdown mode.
  if (!target || phase === "idle") {
    el.textContent = "108:00";      // the idle face, and now only ever in its own mode
    el.className = "big";
    return;
  }
  const now = (Date.now() + skewMs) / 1000;
  const rem = Math.max(0, target - now);
  // The stepped value, not the raw remainder: this readout is the reference the
  // flaps land against (spec 7.3), so showing a live second count while the
  // drums sit on MMM:00 would make the two disagree all through the quiet
  // phase.  The CLOCK rider is a different thing entirely - a screen has no
  // flaps, so realClockFace ignores clock.granularity_min - but the countdown's
  // resolution is the contract both surfaces share.
  const shown = countdownShownS(secondsMode, rem, secondsLive);
  el.textContent = mmss(shown);
  el.className = "big" + (rem <= 60 ? " crit" : rem <= secondsLive ? " warn" : "");
}

// A countdown running while the display shows something else. Both surfaces
// carry it - here and on the control panel's Diagnostics - because "running but
// not on screen" is exactly the state that was invisible.
function updateCountdownChip() {
  const chip = $("cd-chip");
  if (!chip) return;
  const running = !!target && phase !== "idle" && mode !== "countdown";
  chip.style.display = running ? "" : "none";
  if (!running) return;
  const rem = Math.max(0, target - (Date.now() + skewMs) / 1000);
  const shown = phase === "running" ? countdownShownS(secondsMode, rem, secondsLive) : 0;
  chip.textContent = phase === "running"
      ? "COUNTDOWN " + mmss(shown)
      : "COUNTDOWN " + phase.toUpperCase();
}

function onState(s) {
  skewMs = s.t - Date.now();
  if (typeof s.tz_offset_s === "number") tzOffsetS = s.tz_offset_s;
  target = s.cd.target;
  phase = s.cd.phase;
  secondsLive = s.cd.seconds_live_s;
  secondsMode = s.cd.seconds_mode || "seconds";
  $("mode").textContent = s.mode;
  mode = s.mode;
  cueState = s.audio || {};
  timeValid = !!s.time_valid;
  retryMax = (s.sys && s.sys.rehome_retries) || 3;
  // The columns' own reading, so the idle face shows what is on the wall
  // rather than a countdown that is not running.
  // The physical layout (spec 7.1): col 1 is AM/PM, cols 2-3 the hours, cols
  // 4-5 the minutes, with the band between them rendered as the colon.
  // The CRT clock shows REAL time (see realClockFace): granularity exists to
  // save flap wear and a screen has none.  The flap mirror below still follows
  // the real columns, toggle and all - that one is a mirror and must not lie
  // about what the wall is doing.
  h24 = !!(s.cfg && s.cfg.h24);

  // CROSS-REPO CONTRACT (2026-08-24).  "SYSTEM FAILURE" is exactly fourteen
  // characters - six, a single space, seven - with NO trailing space, and the
  // separate terminal prop took this literal from this file.  Phase 7's
  // protocol mode floods the same string.  Three surfaces now print it; if it
  // changes here it changes in another repository too, so it is not a string to
  // tidy.  The flood cadence that goes with it is defined in FIRMWARE_SPEC 7.3.
  const SYSTEM_FAILURE = "SYSTEM FAILURE";
  const sub = {
    idle: "SYSTEM STANDBY",
    running: "ENTER THE NUMBERS TO RESET",
    zero: SYSTEM_FAILURE,
    spin: SYSTEM_FAILURE,
    reveal: SYSTEM_FAILURE,
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

bus.on("state", (s) => {
     onState(s);
     const before = SwanTerm.state ? SwanTerm.state.cd.phase : null;
     SwanTerm.state = s;
     SwanTerm.emit("state", s);
     if (s.cd.phase !== before) SwanTerm.emit("phase", s.cd.phase);
   })
   // The firmware's own announcement that the reveal frame is CONFIRMED on
   // every column - not "commanded".  After the alarm spin the columns converge
   // with the index unknown, measured at 2.45-2.48 s with the canon five, so
   // this is the beat rather than an estimate.
   .on("reveal", (e) => SwanTerm.emit("reveal", e))
   .on("go", (e) => flap && flap.flipTo(e.col, e.idx, e.flaps))
   .on("spin", (e) => flap && flap.spin(e.col, e.flaps, e.secs))
   .on("mode", (e) => { $("mode").textContent = e.name; })
   // "fired", not "played": mute, a volume of 0, quiet hours or a missing WAV
   // all produce silence, and every one of those facts is in the state document
   // this page already receives.
   .on("cue", (e) => {
     setMsg((audioSilent() ? "⃠ " : "♪ ") + e.name.replace(/_/g, " "),
            audioSilent() ? "warn" : "ok");
     SwanTerm.emit("cue", e);
   })
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
     SwanTerm.emit("result", e.res);
   });

// --------------------------------------------------------------------------
// THE HOST SURFACE for the Phase 7 modules (protocol.js, pearl.js, bootanim.js,
// chat.js, chess.js).  They are separate files so each can be read, replaced or
// deleted on its own, and so the friendly terminal keeps working when one of
// them is not loaded at all.
//
// Rules every module obeys, and the reason each one exists:
//   - Screen-side only.  The ONE exception is protocol mode's EXECUTE, which
//     goes through `send`, i.e. the same countdown.execute path this page's own
//     EXECUTE key uses.  Nothing else may send a command, and nothing may touch
//     the flaps.
//   - Time the failure sequence off `target` plus the retained timing keys
//     (cfg.zero_hold_s, cfg.spin_s) and off the `reveal` EVENT for the landing -
//     never off the arrival time of a message.  Publish skew on swan/countdown
//     was measured at up to ~0.7 s (BRINGUP 30), so arrival is not a clock.
//   - Every feature is behind a PREFS toggle that defaults to off.
// --------------------------------------------------------------------------
const SwanTerm = {
  // The last state document, or null before the first arrives.
  state: null,
  prefs: PREFS,
  savePref,
  clickSound,
  setMsg,
  // The one sanctioned command path (protocol mode's EXECUTE only).
  send: (cmd, payload) => bus.send(cmd, payload),
  // The device's wall clock as a Date whose UTC fields are its LOCAL fields.
  deviceNow,
  // Seconds remaining against the device's clock, or null when nothing is armed.
  remaining() {
    if (!target) return null;
    return target - (Date.now() + skewMs) / 1000;
  },
  phase: () => phase,
  mode: () => mode,
  timeValid: () => timeValid,
  secondsLive: () => secondsLive,
  secondsMode: () => secondsMode,
  station: () => stationName,
  setStation,
  applyPrefs,
  // The spec 7.3 rendering contract, shared with protocol mode and pinned by
  // test/host/test_countdown.js.
  shownS: (remainingS) => countdownShownS(secondsMode, remainingS, secondsLive),
  _shownS: countdownShownS,
  _stepS: countdownStepS,

  // Listeners.  "state" on every document, "reveal" when the firmware announces
  // the reveal frame has landed on every column, "phase" on a countdown phase
  // change, "cue" when an audio cue fires.
  _subs: {},
  on(evt, fn) {
    (this._subs[evt] || (this._subs[evt] = [])).push(fn);
    return this;
  },
  emit(evt, arg) {
    const l = this._subs[evt];
    if (!l) return;
    for (const fn of l) {
      try { fn(arg); } catch (e) { console.error("SwanTerm." + evt, e); }
    }
  },
};
window.SwanTerm = SwanTerm;

// RULE 2: THE ESCAPE.  A persisted mode with no way out is a trap, and this is
// a website - a PC user with only a mouse must always be able to leave. So the
// strip hides when untouched (the kiosk pattern) and ANY pointer movement,
// click or tap brings it back for a few seconds.
//
// It is never hidden until the first reveal timer runs, so a browser that drops
// the JS still shows a way out.
const STRIP_MS = 5000;
let stripTimer = 0;

function revealStrip() {
  const f = $("foot");
  if (!f) return;
  f.classList.remove("hidden");
  clearTimeout(stripTimer);
  stripTimer = setTimeout(() => {
    // Only ever auto-hides over the station screen. On the friendly terminal
    // the strip is part of the page and stays put.
    if (PREFS.protocol) f.classList.add("hidden");
  }, STRIP_MS);
}

// The strip's height, published to CSS.  It wraps to three rows on a 375 px
// phone and one on a kiosk, and it floats over the page - so without this the
// mirror dock sits underneath it, which is exactly what it did.
function measureStrip() {
  const f = $("foot");
  if (!f) return;
  document.documentElement.style.setProperty("--foot-h", f.offsetHeight + "px");
}

function wireStrip() {
  // Pointer movement, clicks and taps all count. `pointermove` covers mouse,
  // pen and touch-drag in one listener; `pointerdown` covers the tap and the
  // click on dead space.
  for (const ev of ["pointermove", "pointerdown", "touchstart"]) {
    document.addEventListener(ev, revealStrip, { passive: true });
  }
  // Keyboard users get it too: any key shows the way out exists.
  document.addEventListener("keydown", revealStrip, true);
  revealStrip();

  measureStrip();
  window.addEventListener("resize", measureStrip);
  if (window.ResizeObserver) {
    // Rotating a phone, or the strip re-wrapping when a label changes length.
    new ResizeObserver(measureStrip).observe($("foot"));
  }

  document.querySelectorAll("[data-station]").forEach((b) => {
    b.onclick = () => { setStation(b.dataset.station); clickSound("key"); revealStrip(); };
  });
  const replay = $("replay");
  if (replay) {
    replay.onclick = () => {
      clickSound("key");
      revealStrip();
      if (window.SwanBoot) window.SwanBoot.play({ skipable: true });
    };
  }
}

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
wireStrip();
renderEntry();

// THE BOOT ANIMATION, defined (defect 1, 2026-08-25).
//
// It plays on EVERY load of this page while the station is SWAN, in both
// content modes, always skippable. It is never on the control panel - that
// page does not load this script. Deliberate replays: the strip's REPLAY LOGO
// button, and typing LOGO at the Swan prompt.
//
// Not on reconnect any more. A dropped socket is not a boot, and replaying a
// four-second logo over a working display every time a phone's radio naps is a
// worse behaviour than the one it was meant to soften.
function playBoot() {
  if (stationName !== "swan" || !window.SwanBoot) return Promise.resolve();
  return window.SwanBoot.play({ skipable: true }).catch(() => {});
}

// DEFERRED, AND THAT IS THE THIRD AND ACTUAL CAUSE OF DEFECT 1.  terminal.html
// loads this file at script tag 3 and bootanim.js at tag 7, so a top-level
// call runs before `window.SwanBoot` exists and the guard above returns
// immediately - on every load, whatever the preferences said.  Two gates were
// removed before this was found, and neither removal changed anything, which
// is the lesson: the feature was unreachable by three independent routes and
// fixing the first two looked exactly like fixing none.
//
// DOMContentLoaded is the right moment: classic scripts block the parser, so
// every module in the page has run by then.
if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", playBoot);
} else {
  playBoot();
}

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
