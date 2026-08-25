// The Pearl log printout (spec §15 phase 7).
//
// From the idle prompt: PRINT LOG? Y/N.  On Y this renders the device's own
// persistent event journal as a tractor-feed dot-matrix printout - green-bar
// paper, characters arriving one at a time, a synthesized ImageWriter rattle.
//
// THE POINT OF IT, and the reason it is worth the bytes: this is REAL DEVICE
// HISTORY in show dress.  Every line is an event this display actually recorded
// - countdowns you executed, faults that happened at 3 a.m., the boot after the
// power cut - read from GET /api/journal.  A prop that printed invented lines
// would be easier and would be worth nothing.
//
// THE JOURNAL FORMAT IS A FROZEN CONTRACT (FIRMWARE_SPEC §12) and this renderer
// honours it rather than guessing:
//   - NDJSON, one object per line, NEWEST LAST.
//   - `t` `u` `e` always present; `col` `seq` `by` `d` conditional.
//   - `t` is UTC seconds and **0 MEANS THE CLOCK HAD NEVER SYNCED** - such a
//     line is stamped from `u` (uptime) instead, because printing 1970 would be
//     a lie about when it happened.
//   - `col` is present for column 0 too, so `col` must be tested for undefined
//     rather than for truthiness.
//   - The kinds are exactly: boot execute start reset cancel zero fault recover
//     mode maint column reveal.
//   - `reset` NEVER APPEARS (countdown.reset is an alias of start and journals
//     as `start`), and countdown.set_target writes no line at all.  Both are
//     documented absences, not gaps to paper over.
//
// Screen-side only: one GET, no commands, no flaps.
"use strict";

(function (global) {
  const CPS = 220;                 // characters per second - a fast 9-pin
  const CHUNK_MS = 16;             // one animation frame's worth per tick
  const HEAD_LINES = 6;            // the banner, printed before the entries

  let el = null;
  let parts = null;
  let open = false;
  let typing = null;               // the interval handle
  let full = "";                   // everything that will be printed
  let shown = 0;                   // how much of it has been
  let audioCtx = null;
  let lastRattle = 0;

  const T = () => global.SwanTerm;

  // ---------------------------------------------------------------------
  // The ImageWriter, synthesized.  A dot-matrix head is a burst of pins
  // striking a ribbon: filtered noise, very short, plus the carriage.  No
  // samples to ship and nothing to decode.
  // ---------------------------------------------------------------------
  function ctx() {
    if (audioCtx) return audioCtx;
    const C = global.AudioContext || global.webkitAudioContext;
    if (!C) return null;
    audioCtx = new C();
    return audioCtx;
  }

  function rattle() {
    const t = T();
    if (t && !t.prefs.click) return;      // the same toggle as the key click
    const a = ctx();
    if (!a) return;
    // Rate-limit: at 220 characters a second one burst per character would be
    // both wrong (a print head strikes a column, not a glyph) and a lot of
    // oscillators.
    const now = a.currentTime;
    if (now - lastRattle < 0.028) return;
    lastRattle = now;

    const n = a.createBufferSource();
    const len = Math.floor(a.sampleRate * 0.012);
    const buf = a.createBuffer(1, len, a.sampleRate);
    const d = buf.getChannelData(0);
    for (let i = 0; i < len; i++) {
      d[i] = (Math.random() * 2 - 1) * (1 - i / len);
    }
    n.buffer = buf;
    const bp = a.createBiquadFilter();
    bp.type = "bandpass";
    bp.frequency.value = 2100 + Math.random() * 600;
    bp.Q.value = 1.4;
    const g = a.createGain();
    g.gain.value = 0.035;
    n.connect(bp); bp.connect(g); g.connect(a.destination);
    n.start();
  }

  // ---------------------------------------------------------------------
  // Rendering one journal line into the show's register.
  // ---------------------------------------------------------------------
  function pad(n, w) { return String(n).padStart(w, "0"); }

  // The station's timestamp register.  A synced line gets the device's own wall
  // clock; an unsynced one (t === 0) gets its uptime, marked, because 1970
  // would be a fabrication.
  function stamp(ev) {
    if (ev.t && ev.t > 0) {
      const d = new Date(ev.t * 1000);
      // The DEVICE's zone: the state document carries its offset, and a kiosk
      // in another one must not restamp the display's own history.
      const off = (T() && T().state && T().state.tz_offset_s) || 0;
      const l = new Date(d.getTime() + off * 1000);
      return pad(l.getUTCFullYear(), 4) + "." + pad(l.getUTCMonth() + 1, 2) + "." +
             pad(l.getUTCDate(), 2) + "  " + pad(l.getUTCHours(), 2) + ":" +
             pad(l.getUTCMinutes(), 2) + ":" + pad(l.getUTCSeconds(), 2);
    }
    const u = ev.u || 0;
    return "  UPTIME  " + pad(Math.floor(u / 3600), 3) + ":" +
           pad(Math.floor(u / 60) % 60, 2) + ":" + pad(u % 60, 2);
  }

  function body(ev) {
    const by = ev.by ? " BY " + String(ev.by).toUpperCase() : "";
    const seq = ev.seq ? "  SEQ " + ev.seq : "";
    // `col` is emitted for column 0, so test for undefined - not for truth.
    const col = ev.col === undefined ? "" : "  COLUMN " + (ev.col + 1);
    const d = ev.d ? String(ev.d) : "";
    switch (ev.e) {
      case "boot":
        return "SYSTEM RESTART  " + d.toUpperCase();
      case "execute":
        // The ritual, in the show's word for it.
        return "ACCEPTED  " + d + by + seq;
      case "start":
        return "COUNTDOWN ENGAGED" + by + seq;
      case "cancel":
        return "COUNTDOWN DISENGAGED" + by + seq;
      case "zero":
        return "SYSTEM FAILURE" + seq;
      case "reveal":
        return "DISPLAY SEALED" + seq;
      case "fault":
        return "FAULT" + col + "  " + d.toUpperCase();
      case "recover":
        return "RECOVERED" + col + "  " + d.toUpperCase();
      case "mode":
        return "MODE  " + d.toUpperCase();
      case "maint":
        return "MAINTENANCE  " + d.toUpperCase();
      case "column":
        return "COLUMN CONFIGURED" + col + "  " + d.toUpperCase();
      case "reset":
        // Documented never to appear (§12): countdown.reset is an alias of
        // start.  Rendered anyway, so a future firmware that does emit it is
        // not silently dropped on the floor.
        return "COUNTDOWN RESET" + by + seq;
      default:
        return String(ev.e || "?").toUpperCase() + (d ? "  " + d : "");
    }
  }

  function compose(lines) {
    const now = new Date();
    const out = [];
    out.push("=".repeat(58));
    out.push("  DHARMA INITIATIVE  ·  STATION 3  ·  THE SWAN");
    out.push("  INCIDENT AND OPERATIONS LOG — AUTOMATED TRANSCRIPT");
    out.push("=".repeat(58));
    out.push("");
    if (!lines.length) {
      out.push("  NO ENTRIES ON RECORD.");
    }
    for (const raw of lines) {
      let ev;
      try { ev = JSON.parse(raw); } catch (_) { continue; }
      if (!ev || typeof ev !== "object" || !ev.e) continue;
      out.push(stamp(ev) + "   " + body(ev));
    }
    out.push("");
    out.push("-".repeat(58));
    out.push("  END OF TRANSCRIPT  ·  " + lines.length + " ENTRIES");
    out.push("");
    return out.join("\n");
  }

  // ---------------------------------------------------------------------
  function build() {
    if (el) return;
    el = document.createElement("div");
    el.className = "pearl";
    el.innerHTML =
      '<div class="pearl-paper"><pre class="pearl-text" id="pearl-text"></pre></div>' +
      '<div class="pearl-foot">' +
      '<button class="tbtn" id="pearl-save">SAVE .TXT</button>' +
      '<button class="tbtn" id="pearl-close">CLOSE</button>' +
      '<span class="pearl-hint" id="pearl-hint"></span></div>';
    document.body.appendChild(el);
    parts = {
      text: el.querySelector("#pearl-text"),
      hint: el.querySelector("#pearl-hint"),
    };
    el.querySelector("#pearl-close").onclick = () => close();
    el.querySelector("#pearl-save").onclick = () => save();
  }

  function save() {
    // A plain-text copy of exactly what was printed.  Same content, no dress.
    const blob = new Blob([full], { type: "text/plain" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "swan-log.txt";
    document.body.appendChild(a);
    a.click();
    setTimeout(() => {
      URL.revokeObjectURL(a.href);
      a.remove();
    }, 1000);
  }

  function startTyping() {
    const reduce = global.matchMedia &&
                   global.matchMedia("(prefers-reduced-motion: reduce)").matches;
    if (reduce) {
      shown = full.length;
      parts.text.textContent = full;
      parts.hint.textContent = "PRINTED";
      return;
    }
    const per = Math.max(1, Math.round((CPS * CHUNK_MS) / 1000));
    typing = setInterval(() => {
      if (shown >= full.length) {
        stopTyping();
        parts.hint.textContent = "PRINTED";
        return;
      }
      shown = Math.min(full.length, shown + per);
      parts.text.textContent = full.slice(0, shown);
      // Paper feed: the platen always shows the line being struck.
      parts.text.parentNode.scrollTop = parts.text.parentNode.scrollHeight;
      rattle();
    }, CHUNK_MS);
  }

  function stopTyping() {
    if (typing !== null) clearInterval(typing);
    typing = null;
  }

  function openLog() {
    if (open) return Promise.resolve();
    open = true;
    build();
    el.classList.add("on");
    parts.text.textContent = "";
    parts.hint.textContent = "READING JOURNAL…";
    shown = 0;
    full = "";

    // ONE GET, and the only network this whole phase does.
    return fetch("/api/journal")
      .then((r) => (r.ok ? r.text() : Promise.reject(new Error(String(r.status)))))
      .then((txt) => {
        const lines = txt.split("\n").map((l) => l.trim()).filter((l) => l.length > 1);
        full = compose(lines);
        parts.hint.textContent = "PRINTING…";
        startTyping();
      })
      .catch((e) => {
        full = compose([]) + "\n  JOURNAL UNAVAILABLE: " + e.message + "\n";
        parts.text.textContent = full;
        shown = full.length;
        parts.hint.textContent = "NO LINK";
      });
  }

  function close() {
    stopTyping();
    open = false;
    if (el) el.classList.remove("on");
  }

  // Skip to the end rather than closing, on any key while it prints: somebody
  // who wants the whole log should not have to wait for the animation, and
  // somebody who wants out presses CLOSE or ESC.
  document.addEventListener("keydown", (e) => {
    if (!open) return;
    if (e.key === "Escape") { close(); return; }
    if (typing !== null) {
      stopTyping();
      shown = full.length;
      parts.text.textContent = full;
      parts.text.parentNode.scrollTop = parts.text.parentNode.scrollHeight;
      parts.hint.textContent = "PRINTED";
    }
  }, true);

  // TYPE LOG - THE PEARL'S ONE COMMAND (2026-08-25).
  //
  // The station screen handles this itself while it is up; this sniffer is for
  // the friendly terminal, so selecting PEARL on the strip means the same thing
  // in both content modes.  It is deliberately the same shape as the Flame's:
  // a word typed at an idle prompt, scoped to its own station, and off
  // everywhere else.
  const LOG_WORD = "LOG";
  let logTyped = "";
  let logAt = 0;

  document.addEventListener("keydown", (e) => {
    if (open || e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
    const t = global.SwanTerm;
    if (!t || !t.station || t.station() !== "pearl") return;
    const p = global.SwanProtocol;
    if (p && p.isOn && p.isOn()) return;         // the station screen owns the keys
    if (!e.key || e.key.length !== 1) return;
    const c = e.key.toUpperCase();
    if (c < "A" || c > "Z") { logTyped = ""; return; }

    const now = Date.now();
    if (now - logAt > 2000) logTyped = "";
    logAt = now;
    const next = logTyped + c;
    logTyped = LOG_WORD.indexOf(next) === 0 ? next
             : (LOG_WORD.indexOf(c) === 0 ? c : "");
    if (logTyped === LOG_WORD) { logTyped = ""; openLog(); }
  }, false);

  global.SwanPearl = {
    open: openLog,
    close,
    isOpen: () => open,
    // Exposed so the renderer's contract handling is testable without a device.
    _compose: compose,
    _body: body,
    _stamp: stamp,
  };
})(window);
