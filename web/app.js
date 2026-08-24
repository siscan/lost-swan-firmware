// LOST Swan split-flap - web UI (spec 10.2).
//
// Every control on every page sends a §10.2a command through /ws (or POST
// /api/cmd when the socket is down).  The one exception is the ring upload, which POSTs the document to /api/ring/upload rather than issuing ring.upload - that route adds a heap guard the dispatcher command does not have: what
// this page can do, an MQTT publish can do, and the firmware validates all of
// it identically.
"use strict";

const $ = (id) => document.getElementById(id);
const el = (tag, attrs, text) => {
  const n = document.createElement(tag);
  if (attrs) for (const k in attrs) n.setAttribute(k, attrs[k]);
  if (text !== undefined) n.textContent = text;
  return n;
};

let ring = null;       // GET /api/ring
let flap = null;       // the mirror
let state = null;      // the last /ws state document
// Whether THIS element is the one being edited.  There used to be a
// page-global `cfgDirty` latched by focus/blur listeners attached once in
// wire(), which had two faults: a single focused checkbox froze the whole
// Settings render, and any control built later - the reveal pickers, the
// per-column mode selects - was never in that query and so was never
// protected at all.  Asking the element is both narrower and complete.
const editing = (node) => document.activeElement === node;
// The device's own REHOME_RETRIES, not a copy of it.
const retryMax = () => (state && state.sys && state.sys.rehome_retries) || 3;
let wear = null;       // GET /api/wear, refetched when an input to it changes
let wearKey = "";      // the (h24, live_s) the current table was computed for

// --------------------------------------------------------------------------
// Transport - bus.js, shared with the presentation terminal
// --------------------------------------------------------------------------
function toast(msg, ok) {
  const t = $("toast");
  t.textContent = msg;
  t.className = "show " + (ok ? "ok" : "err");
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { t.className = ""; }, 2600);
}

const bus = new SwanBus({
  onstatus: (up) => {
    $("link").textContent = up ? "connected" : "disconnected";
    $("link").className = up ? "ok" : "bad";
    // The FlapDisplay outlives a reconnect, so without this the first document
    // after a gap would animate the catch-up one flap at a time.  Re-priming
    // snaps instead, which is what a page that has just found the display
    // again should do.
    if (!up && flap) flap.primed = false;
  },
});

const send = (cmd, payload) => bus.send(cmd, payload);

bus.on("state", (e) => onState(e))
   .on("go", (e) => flap && flap.flipTo(e.col, e.idx, e.flaps))
   .on("spin", (e) => flap && flap.spin(e.col, e.flaps, e.secs))
   .on("mode", (e) => { $("s-mode").textContent = e.name; })
   .on("cue", (e) => toast("♪ " + e.name, true))
   .on("result", (e) => {
     if (!e.res) return;
     if (e.res.ok === false) {
       // "rejected" alone is what the Numbers ritual returns, and on the
       // Terminal page it is the one refusal that needs saying in words.
       toast(e.res.err === "rejected" ? "wrong numbers" : (e.res.err || "rejected"), false);
     } else if (e.res.note) {
       // Accepted, but not the thing you were probably after - a nudge on an
       // unhomed column, a deadline armed while maintenance holds the display.
       toast(e.res.note, true);
     }
   });

// --------------------------------------------------------------------------
// State rendering
// --------------------------------------------------------------------------
function mmss(total) {
  if (total < 0) total = 0;
  const m = Math.floor(total / 60);
  const s = total % 60;
  return String(m).padStart(3, "0") + ":" + String(s).padStart(2, "0");
}

function onState(s) {
  state = s;
  $("s-mode").textContent = s.mode;
  // The deadline survives a cancel, so only show remaining while it means
  // something - an idle countdown is idle, not "107:56 idle".
  const cdLive = s.cd.phase === "running" || s.cd.phase === "zero" || s.cd.phase === "spin";
  $("s-cd").textContent = s.cd.phase + (cdLive ? " · " + mmss(s.cd.remaining_s) : "");
  $("s-clock").textContent = s.time_valid
      ? new Date(s.t).toLocaleTimeString()
      : "no time (" + (s.wifi_glyph ? "wifi glyph up" : "waiting for SNTP") + ")";
  $("s-ring").textContent = s.ring.source + " · " + s.ring.slots +
      (s.ring.descending ? " · descending" : " · NOT DESCENDING");
  $("t-remaining").textContent = cdLive ? mmss(s.cd.remaining_s) : "—";

  // The mirror follows the axes.  A page opened mid-run SNAPS once to where
  // the drums are, without animating fifty phantom flips; from then on every
  // state document reconciles it, because the go events that drive the
  // animation are lossy and nothing else would ever correct a card that
  // missed one.  A column with index -1 paints as UNKNOWN, not as the blank
  // flap - they are different facts.
  if (flap && !flap.primed) {
    flap.setAll(s.cols.map((c) => c.index));
    flap.primed = true;
  }
  if (flap) {
    flap.setStates(s.cols);
    flap.reconcile(s.cols, s.cfg && s.cfg.flaps_s_normal);
  }
  renderMotion(s);

  renderDiag(s);
  renderCal(s);
  renderSettings(s);

  $("ramp-state").textContent = s.cal.ramp_active
      ? "walking column " + (s.cal.ramp_col + 1)
      : "";
}

// The banner.  Any column not settled is worth saying on every page, with the
// retry count, because a column thrashing through three re-homes and a column
// quietly parked look identical otherwise.
function renderMotion(s) {
  renderRig(s);
  const el = $("motion");
  const m = s.motion || {};
  // A disabled column is EXPECTED to sit still.  Reporting it as "not settled"
  // would train you to ignore the banner, which is the one thing it must never
  // become. It is reported by the rig strip instead, as configuration.
  const busy = s.cols
    .map((c, i) => ({ i, c }))
    .filter(({ c }) => c.mode !== "disabled" && (c.state !== "IDLE" || c.index < 0));
  if (busy.length === 0 || m.maintenance) {
    el.className = "";
    el.textContent = "";
    return;
  }
  const faulted = busy.some(({ c }) => c.state === "FAULT");
  // "fault (re-home 3/3)" read as still-trying on the bench.  A column that
  // has given up and one that is mid-retry are different problems and must
  // read differently.
  const parts = busy.map(({ i, c }) => {
    const n = "col " + (i + 1);
    if (c.state === "FAULT") {
      // The cause changes what you should DO, so it is in the banner, not
      // buried in Diagnostics: a jam means stop touching the start button.
      if (c.cause === "jam") return n + " JAMMED (stopped, not retried)";
      const why = c.cause === "no_hall" ? " no hall edge — sensor, magnet or wiring"
                : c.cause === "slip"    ? " lost registration"
                : "";
      return n + " FAULT" + why +
             (c.retry > 0 ? " (gave up after " + c.retry +
                            (c.retry === 1 ? " re-home)" : " re-homes)")
                          : " (not retried)");
    }
    if (c.state === "HOMING") {
      return n + (c.retry > 0 ? " re-homing " + c.retry + "/" + retryMax() : " homing");
    }
    if (c.state === "UNHOMED") return n + " unhomed";
    if (c.index < 0) return n + " position unknown";
    return n + " " + c.state.toLowerCase();
  });
  const jammed = busy.some(({ c }) => c.cause === "jam");
  el.className = "show" + (faulted ? " bad" : "");
  el.innerHTML = (jammed ? "MECHANICAL — " : faulted ? "MOTION FAULT — " : "COLUMNS NOT SETTLED — ") +
      "<b>" + parts.join(" · ") + "</b>" +
      (jammed ? ". The drum stopped while the motor kept stepping. Clear the "
                + "obstruction before re-homing — retrying drives the motor into it."
       : faulted ? ". `home &lt;col&gt;` on the console, or REHOME on the Calibrate page."
                 : ". A homing pass takes ~7.5 s; a column tries three times before "
                   + "giving up, so allow ~30 s from boot.");
}

// The rig strip: what this display IS, as opposed to how it is doing.  Sits
// above the banner and is deliberately impossible to miss, because a simulated
// display that looks real is worse than no display at all.
function renderRig(s) {
  const el = $("rig");
  const m = s.motion || {};
  const bits = [];
  if (m.maintenance) bits.push("MAINTENANCE — nothing moves on its own");
  if (m.sim_columns > 0) {
    const which = s.cols.map((c, i) => (c.mode === "sim" ? i + 1 : 0)).filter(Boolean);
    bits.push("SIMULATED MOTION on col " + which.join(", ") +
              " — not driving real hardware");
  }
  if (m.disabled_columns > 0) {
    const which = s.cols.map((c, i) => (c.mode === "disabled" ? i + 1 : 0)).filter(Boolean);
    bits.push("col " + which.join(", ") + " DISABLED — parked, excluded from frames");
  }
  el.className = bits.length ? "show" : "";
  el.innerHTML = bits.map((b) => "<b>" + b + "</b>").join(" · ");
}

function renderDiag(s) {
  const body = $("diag-rows");
  body.textContent = "";
  s.cols.forEach((c, i) => {
    const tr = el("tr");
    const cells = [
      [String(i + 1), false], [c.state, false], [c.face, false],
      [String(c.index), true], [String(c.dest), true], [String(c.cal), true],
      [String(c.revs), true], [String(c.flips), true], [String(c.minor), true],
      [String(c.major), true], [String(c.faults), true], [String(c.h2h), true],
      [String(c.err), true], [c.hall ? "●" : "○", false],
      [c.retry > 0 ? c.retry + "/" + retryMax() : "—", false],
    ];
    cells.forEach(([text, num]) => tr.appendChild(el("td", num ? { class: "num" } : null, text)));
    body.appendChild(tr);
  });

  renderOta(s);

  const sys = $("diag-sys");
  sys.textContent = "";
  const mq = s.mqtt || {};
  const prov = s.prov || {};
  const au = s.audio || {};
  const facts = [
    ["wifi", s.sys.wifi + (s.sys.ssid ? " · " + s.sys.ssid : "")],
    ["address", (s.sys.ip || "—") + " · " + s.sys.host + ".local"],
    // 0 dBm is not a reading, it is the absence of one, and it renders as a
    // perfect signal if you print it anyway.
    ["rssi", s.sys.wifi === "connected" ? s.sys.rssi + " dBm" : "—"],
    // Total free heap does not decide whether an upload is refused; the largest
    // contiguous block does, and that is the number the guards test.
    ["heap", s.sys.heap + " B  (largest block " + s.sys.heap_largest + ")"],
    ["image", s.sys.version + " · " + (s.sys.ota_partition || "?") +
              (s.sys.ota_pending ? " · PENDING VERIFICATION" : "")],
    ["mqtt", !mq.enabled ? "off"
             : (mq.connected ? "connected · " + mq.base : "enabled, NOT connected") +
               (mq.dropped ? " · " + mq.dropped + " dropped" : "")],
    ["portal", prov.portal ? "up: " + prov.ssid
                           : (prov.configured ? "down" : "down · no credentials")],
    ["audio", (au.cues_present === au.cues_total
                 ? au.cues_total + " cues"
                 : au.cues_present + "/" + au.cues_total + " cues — ONE IS MISSING") +
              " · vol " + au.volume + (au.mute ? " · MUTED" : "")],
    ["countdown", s.cd.phase + (s.cd.set_by && s.cd.set_by !== "unknown"
                                    ? " · set by " + s.cd.set_by + " (seq " + s.cd.seq + ")"
                                    : "")],
    // Non-zero means the board could not push everything it wanted to.  The
    // mirror still tracks - it reconciles against this very document - but the
    // flip animations for the dropped events were lost, and a rising count
    // means the transport is under pressure.
    ["ws dropped", s.sys.ws_dropped],
    ["uptime", s.sys.uptime_s + " s"],
    ["reset reason", s.sys.reset],
    ["firmware", s.sys.version],
    ["time valid", String(s.time_valid)],
  ];
  facts.forEach(([k, v]) => {
    const card = el("div", { class: "card" });
    card.appendChild(el("h3", null, k));
    card.appendChild(el("div", null, v));
    sys.appendChild(card);
  });
}

// --------------------------------------------------------------------------
// Pickers.  A column may only be offered what ITS OWN ring carries - column 5
// is a different part number with a different glyph set (spec 4).
// --------------------------------------------------------------------------
function glyphSelect(col, withBlank) {
  const sel = el("select", { "data-col": String(col) });
  if (withBlank) sel.appendChild(el("option", { value: "_" }, "— blank —"));
  else sel.appendChild(el("option", { value: "" }, "— unset —"));
  const c = ring.columns[col] || {};
  (c.glyphs || []).forEach((g) => {
    sel.appendChild(el("option", { value: g.id }, g.label || g.id));
  });
  if (withBlank) {
    // Digits and AM/PM are legitimate message tokens too.
    (c.ring || []).forEach((slot) => {
      if (slot.cat === "digit" || slot.cat === "ampm" || slot.cat === "wifi") {
        sel.appendChild(el("option", { value: slot.id }, slot.label || slot.id));
      }
    });
  }
  return sel;
}

function buildPickers() {
  const msg = $("msg-pickers");
  msg.textContent = "";
  const rev = $("reveal-pickers");
  rev.textContent = "";
  for (let i = 0; i < ring.columns.length; i++) {
    const w1 = el("div");
    w1.appendChild(el("label", null, "col " + (i + 1) + " "));
    w1.appendChild(glyphSelect(i, true));
    msg.appendChild(w1);

    const w2 = el("div");
    w2.appendChild(el("label", null, "col " + (i + 1) + " "));
    const s = glyphSelect(i, false);
    s.className = "reveal-sel";
    s.onchange = pushReveal;
    w2.appendChild(s);
    rev.appendChild(w2);
  }

  const slots = ring.columns[0].slots;
  ["ramp-from", "ramp-to", "ramp-step"].forEach((id) => {
    $(id).max = String(slots - 1);
  });
  $("ramp-to").value = String(slots - 1);

  ["ramp-col", "spin-col"].forEach((id) => {
    const sel = $(id);
    sel.textContent = "";
    for (let i = 0; i < ring.columns.length; i++) {
      sel.appendChild(el("option", { value: String(i) }, "column " + (i + 1)));
    }
  });
}

function pushReveal() {
  const names = Array.from(document.querySelectorAll(".reveal-sel"))
      .map((s) => (s.value === "" ? null : s.value));
  send("config.set", { reveal: names });
}

// --------------------------------------------------------------------------
// Calibrate
// --------------------------------------------------------------------------
function buildCal() {
  const host = $("cal-cards");
  host.textContent = "";
  for (let i = 0; i < ring.columns.length; i++) {
    const card = el("div", { class: "card" });
    card.appendChild(el("h3", null, "COLUMN " + (i + 1)));
    card.appendChild(el("div", { id: "cal-face-" + i }, "—"));
    card.appendChild(el("div", { id: "cal-off-" + i, class: "hint" }, ""));
    const row = el("div", { class: "row" });
    [[-10, "−10"], [-1, "−1"], [1, "+1"], [10, "+10"]].forEach(([d, label]) => {
      const b = el("button", { class: "small" }, label);
      b.onclick = () => send("motion.cal", { column: i, delta: d });
      row.appendChild(b);
    });
    const rh = el("button", { class: "small" }, "REHOME");
    rh.onclick = () => send("motion.rehome", { column: i });
    row.appendChild(rh);
    card.appendChild(row);
    host.appendChild(card);
  }
}

function renderCal(s) {
  s.cols.forEach((c, i) => {
    const f = $("cal-face-" + i);
    const o = $("cal-off-" + i);
    if (f) f.textContent = c.face + "  (slot " + c.index + ")";
    if (o) o.textContent = "cal " + c.cal + " µsteps · " + c.state +
                           " · h2h " + c.h2h;
  });
}

// --------------------------------------------------------------------------
// Settings + live speed sliders
// --------------------------------------------------------------------------
const SLIDERS = [
  ["p-normal", "v-normal", "flaps_s_normal"],
  ["p-alarm", "v-alarm", "flaps_s_alarm"],
  ["p-home", "v-home", "flaps_s_home"],
  ["p-accel", "v-accel", "accel"],
  ["p-halltol", "v-halltol", "hall_tol"],
];

function renderSettings(s) {
  // Every field guarded the same way: the one being edited is left alone, the
  // rest track the device.  Uniform on purpose - the old mixture of guarded and
  // unguarded fields is why a page-global latch was needed to cover the gaps.
  const setVal = (id, v) => { const n = $(id); if (!editing(n)) n.value = v; };
  const setChk = (id, v) => { const n = $(id); if (!editing(n)) n.checked = v; };
  setChk("set-h24", s.cfg.h24);
  setVal("set-tz", s.cfg.tz);
  setVal("set-gran", s.cfg.granularity_min);
  setVal("set-secmode", s.cfg.seconds_mode);
  setVal("set-live", s.cfg.seconds_live_s);
  setVal("set-zero", s.cfg.zero_hold_s);
  setVal("set-spin", s.cfg.spin_s);
  setVal("set-ftimeout", s.cfg.failure_timeout_s);
  setVal("set-floop", s.cfg.failure_loop_s);
  setVal("set-ntp", s.cfg.ntp);
  setChk("set-cdland", s.cfg.cd_land_on_tick);
  setChk("set-clockland", s.cfg.clock_land_on_tick);
  setVal("set-dwell", s.cfg.msg_dwell_s);
  loadWear(s.cfg, false).then(() => renderWear(s.cfg));

  SLIDERS.forEach(([slider, out, key]) => {
    const node = $(slider);
    if (!editing(node)) node.value = s.cfg[key];
    $(out).textContent = node.value;
  });

  const sel = document.querySelectorAll(".reveal-sel");
  s.cfg.reveal.forEach((name, i) => {
    if (sel[i] && !editing(sel[i])) sel[i].value = name === null ? "" : name;
  });

  $("ring-info").textContent = "loaded from " + s.ring.source + " · " +
      s.ring.slots + " slots per drum";

  renderNetwork(s);
  renderAudio(s);
  // The dwell field used to be a hard-coded 600 in the markup, silently
  // overriding the configured msg.dwell_s on the first message anyone sent.
  if (!editing($("msg-dwell"))) $("msg-dwell").value = s.cfg.msg_dwell_s;
  // ... and the pickers never read back the live message, though it is on the
  // wire: opening Modes on a display already showing one offered five blanks.
  if (s.msg) {
    const toks = String(s.msg).split(" ");
    const sel = document.querySelectorAll("#msg-pickers select");
    if (sel.length === toks.length) {
      toks.forEach((t, i) => { if (!editing(sel[i])) sel[i].value = t; });
    }
  }
  const revealSet = (s.cfg.reveal || []).some((n) => n !== null);
  $("reveal-preset-hint").textContent = revealSet
      ? ""
      : "REVEAL is five blanks until the reveal glyphs are chosen, below in Settings.";
  if (!editing($("set-enidle"))) $("set-enidle").checked = !!s.cfg.en_idle_off;

  renderColumnModes(s);
  setChk("set-maint", !!(s.motion && s.motion.maintenance));
  $("maint-hint").textContent = s.motion && s.motion.maintenance
      ? "suspended — nothing is scheduled and nothing re-homes"
      : "";
}

// Audio (spec 9).  Volume, mute, quiet hours and one row per cue - name,
// whether the file is there, HOW LONG it is, and play/stop/replace.
const hhmm = (min) => String(Math.floor(min / 60)).padStart(2, "0") + ":" +
                      String(min % 60).padStart(2, "0");

function renderAudio(s) {
  const a = s.audio || {};
  if (!editing($("set-vol"))) $("set-vol").value = a.volume;
  $("v-vol").textContent = $("set-vol").value;
  if (!editing($("set-mute"))) $("set-mute").checked = !!a.mute;

  const quiet = a.quiet_start_min !== a.quiet_end_min;
  if (!editing($("set-quiet-start"))) $("set-quiet-start").value = hhmm(a.quiet_start_min || 0);
  if (!editing($("set-quiet-end"))) $("set-quiet-end").value = hhmm(a.quiet_end_min || 0);
  $("quiet-hint").textContent = quiet ? "" : "off (both the same)";

  const host = $("cue-rows");
  const cues = a.cues || [];
  // Rebuilt only when the set changes - a file input must not be recreated
  // underneath somebody who has just picked a file.
  if (host.childElementCount !== cues.length) {
    host.textContent = "";
    cues.forEach((c) => {
      const row = el("div", { class: "row", id: "cue-" + c.name });
      row.appendChild(el("b", { style: "min-width:9rem;display:inline-block" }, c.name));
      row.appendChild(el("span", { class: "hint", id: "cue-state-" + c.name }, ""));
      const play = el("button", null, "PLAY");
      play.onclick = () => send("audio.play", c.name);
      row.appendChild(play);
      const stop = el("button", null, "STOP");
      stop.onclick = () => send("audio.stop");
      row.appendChild(stop);
      const file = el("input", { type: "file", accept: ".wav,audio/wav", id: "cue-file-" + c.name });
      row.appendChild(file);
      const up = el("button", null, "REPLACE");
      up.onclick = () => uploadCue(c.name);
      row.appendChild(up);
      host.appendChild(row);
    });
  }
  cues.forEach((c) => {
    const st = $("cue-state-" + c.name);
    if (!st) return;
    const playing = a.playing && a.cue === c.name;
    st.textContent = (c.present ? (c.ms / 1000).toFixed(2) + " s" : "MISSING") +
                     (playing ? "  ♪ playing" : "");
    st.className = c.present ? "hint" : "hint warn";
  });
}

function uploadCue(name) {
  const f = $("cue-file-" + name).files[0];
  if (!f) { toast("choose a .wav for " + name + " first", false); return; }
  fetch("/api/audio/" + name, { method: "POST", body: f })
    .then((r) => r.json().catch(() => ({ ok: false, err: "HTTP " + r.status })))
    .then((r) => {
      if (r.ok) toast(name + " replaced: " + r.rate + " Hz, " + r.bytes + " bytes", true);
      else toast("rejected: " + (r.err || "?"), false);
    })
    .catch(() => toast("upload failed", false));
}

// WiFi, the portal and MQTT.  All three publish state on every document and
// none of it was rendered - so a display could be publishing to a broker, or
// beaconing an open access point, with nothing on any page saying so.
function renderNetwork(s) {
  const sys = s.sys || {};
  const prov = s.prov || {};
  const mq = s.mqtt || {};

  $("wifi-status").textContent =
      sys.wifi === "connected"
          ? "connected to " + (sys.ssid || "?") + " · " + sys.ip + " · " + sys.rssi + " dBm"
          : (prov.configured ? "not connected (" + sys.wifi + ")"
                             : "no credentials stored — this display is a standalone clock");
  if (!editing($("set-ssid"))) $("set-ssid").value = sys.ssid || "";

  $("btn-portal").textContent = prov.portal ? "STOP THE SETUP PORTAL"
                                            : "START THE SETUP PORTAL";
  $("portal-hint").textContent = prov.portal
      ? "up: join \"" + prov.ssid + "\" and a sign-in page should appear"
      : "";

  // The one readout that says whether the canonical external API is actually
  // working.  "enabled" is a setting; "connected" is a fact.
  $("mqtt-status").textContent = !mq.enabled
      ? "off"
      : (mq.connected ? "connected to " + mq.uri + " as " + mq.base
                      : "enabled but NOT connected to " + mq.uri) +
        (mq.dropped ? " · " + mq.dropped + " dropped" : "");
  const setIf = (id, v) => { const n = $(id); if (!editing(n)) n.value = v; };
  const chkIf = (id, v) => { const n = $(id); if (!editing(n)) n.checked = v; };
  chkIf("set-mqtt-en", !!mq.enabled);
  setIf("set-mqtt-uri", mq.uri || "");
  setIf("set-mqtt-base", mq.base || "");
  setIf("set-mqtt-user", mq.user || "");
  setIf("set-mqtt-hap", mq.ha_prefix || "");
  // The password box is deliberately never written back - there is nothing to
  // write it back FROM, and an empty box is what tells the firmware to keep it.
}

// One row per column: what it is, and what it is doing.  Built once and then
// only updated, so a <select> the pointer is inside is never rebuilt underneath.
function renderColumnModes(s) {
  const host = $("col-modes");
  const N_COLS = s.cols.length;
  if (host.children.length !== N_COLS) {
    host.innerHTML = "";
    for (let i = 0; i < N_COLS; ++i) {
      const row = el("div", { class: "row" });
      row.appendChild(el("label", { for: "col-mode-" + i }, "column " + (i + 1)));
      const sel = el("select", { id: "col-mode-" + i, class: "col-mode" });
      [["real", "real — drives the hardware"],
       ["sim", "sim — modelled drum"],
       ["disabled", "disabled — parked, left out of frames"]].forEach(([v, t]) => {
        const o = el("option", { value: v }, t);
        sel.appendChild(o);
      });
      sel.addEventListener("change", () => {
        send("motion.column", { column: i, mode: sel.value });
      });
      row.appendChild(sel);
      row.appendChild(el("span", { class: "hint", id: "col-mode-st-" + i }));
      host.appendChild(row);
    }
  }
  for (let i = 0; i < N_COLS; ++i) {
    const sel = $("col-mode-" + i);
    const c = s.cols[i];
    if (!editing(sel)) sel.value = c.mode;
    // Only the "sim" option is gated by the build: greying out the whole
    // control would hide which of the three is unavailable and why.
    const opt = sel.querySelector('option[value="sim"]');
    if (opt) opt.disabled = !(s.motion && s.motion.sim_available);
    $("col-mode-st-" + i).textContent =
        c.mode === "disabled" ? "parked, not homed" :
        c.state === "FAULT" ? "FAULT · " + c.cause :
        c.state === "IDLE" && c.index >= 0 ? "settled on " + c.face :
        c.state.toLowerCase() + (c.retry > 0 ? " (attempt " + c.retry + "/3)" : "");
  }
  $("col-modes-hint").textContent =
      s.motion && s.motion.sim_available ? "" : "this image has no simulated axes compiled in";
}

// Wear comes from the device, which walks a whole day and a whole run through
// the REAL renderer against the loaded ring.  Nothing is interpolated and
// nothing is hard-coded here, so the figure cannot drift from the renderer and
// it follows a ring upload.
function fmt(n) {
  return n.toLocaleString();
}

function wearFor(granularity) {
  if (!wear) return null;
  return (wear.clock || []).find((e) => e.granularity_min === granularity) || null;
}

function cdWearFor(mode) {
  if (!wear) return null;
  return (wear.countdown || []).find((e) => e.mode === mode) || null;
}

function colBreakdown(w) {
  return w.cols.map((v, i) => "col" + (i + 1) + " " + fmt(v)).join(" · ");
}

function renderWear(cfg) {
  const g = wearFor(cfg.granularity_min);
  $("gran-hint").textContent = g ? fmt(g.wear.total) + " flips/day" : "…";
  $("wear-detail").textContent = g
      ? colBreakdown(g.wear) + "  (nominal 24 h; a DST day differs slightly)"
      : "";

  const c = cdWearFor(cfg.seconds_mode);
  $("cd-wear-hint").textContent = c ? fmt(c.wear.total) + " flips/run" : "…";
  $("cd-wear-detail").textContent = c ? colBreakdown(c.wear) : "";

  // Every granularity on the dropdown carries its own measured cost.
  const sel = $("set-gran");
  Array.from(sel.options).forEach((opt) => {
    const e = wearFor(parseInt(opt.value, 10));
    opt.textContent = opt.value + (e ? "  —  " + fmt(e.wear.total) + " flips/day" : "");
  });
}

// The table depends on h24 and seconds_live_s, so it is refetched when either
// moves - and on a ring upload, which changes what a flip costs.
function loadWear(cfg, force) {
  const key = (cfg.h24 ? "24" : "12") + ":" + cfg.seconds_live_s;
  if (!force && key === wearKey) return Promise.resolve();
  wearKey = key;
  return fetch("/api/wear")
    .then((r) => r.json())
    .then((doc) => {
      wear = doc;
      renderWear(cfg);
    })
    .catch(() => { wearKey = ""; });
}

function pushConfig(patch) {
  send("config.set", patch);
}

// --------------------------------------------------------------------------
// Update (spec 10.4)
// --------------------------------------------------------------------------
// The two OTA facts the firmware publishes on every state document and nothing
// rendered: which slot is executing, and whether it has confirmed itself.  A
// display inside the 120 s mark-valid window, about to roll back, looked
// completely normal from every page.
function renderOta(s) {
  const host = $("ota-facts");
  if (!host) return;
  const pending = !!s.sys.ota_pending;
  const facts = [
    ["firmware", s.sys.version],
    ["slot", s.sys.ota_partition || "—"],
    ["state", pending ? "PENDING VERIFICATION" : "confirmed"],
  ];
  host.textContent = "";
  facts.forEach(([k, v]) => {
    const card = el("div", { class: "card" });
    card.appendChild(el("h3", null, k));
    card.appendChild(el("div", null, String(v)));
    host.appendChild(card);
  });
  $("ota-pending").hidden = !pending;
  $("reboot-hint").textContent = pending
      ? "this image is still pending — rebooting now rolls it back"
      : "";
}

// XMLHttpRequest rather than fetch: fetch reports no upload progress, and a
// 1.5 MB image over WiFi is long enough that a page with no feedback looks
// hung — on the one operation during which the display genuinely is holding
// still and cannot answer anything else.
function uploadFirmware(file, force) {
  const msg = $("ota-msg");
  const bar = $("ota-progress");
  bar.hidden = false;
  bar.value = 0;
  msg.textContent = "uploading " + Math.round(file.size / 1024) + " KB…";
  $("btn-ota-upload").disabled = true;

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/api/ota" + (force ? "?force=1" : ""));
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      bar.value = Math.round((e.loaded / e.total) * 100);
      msg.textContent = bar.value + "%";
    }
  };
  xhr.onload = () => {
    let r = {};
    try { r = JSON.parse(xhr.responseText); } catch (_) { /* keep the status line */ }
    $("btn-ota-upload").disabled = false;
    if (r.ok) {
      bar.value = 100;
      msg.textContent = "written to " + r.partition + " (" + r.bytes +
                        " bytes) — rebooting; it must confirm itself or it rolls back";
      toast("update written; rebooting", true);
      // The board reboots ~1 s after replying, so the socket drops and bus.js
      // reconnects on its own.  Nothing to poll for here.
    } else {
      bar.hidden = true;
      msg.textContent = "refused (" + (r.verdict || xhr.status) + "): " + (r.err || xhr.statusText);
      toast("update refused: " + (r.err || xhr.status), false);
    }
  };
  xhr.onerror = () => {
    $("btn-ota-upload").disabled = false;
    bar.hidden = true;
    msg.textContent = "the upload failed before the display answered";
  };
  xhr.send(file);
}

// --------------------------------------------------------------------------
// Wiring
// --------------------------------------------------------------------------
function wire() {
  // Nav.
  document.querySelectorAll("nav button").forEach((b) => {
    b.onclick = () => {
      document.querySelectorAll("nav button").forEach((x) => x.classList.remove("active"));
      document.querySelectorAll("main section").forEach((x) => x.classList.remove("active"));
      b.classList.add("active");
      $("page-" + b.dataset.page).classList.add("active");
    };
  });

  // Every button carrying a literal command.
  document.querySelectorAll("[data-cmd]").forEach((b) => {
    const spec = JSON.parse(b.dataset.cmd);
    b.onclick = () => send(spec.cmd, spec.payload);
  });

  $("btn-execute").onclick = () => {
    if (state && !state.time_valid) {
      toast("the clock has not synced yet - the deadline cannot be set", false);
      return;
    }
    send("countdown.execute", $("numbers").value.trim());
  };
  $("numbers").addEventListener("keydown", (e) => {
    if (e.key === "Enter") $("btn-execute").click();
  });
  $("btn-cancel").onclick = () => send("countdown.cancel");

  $("btn-message").onclick = () => {
    const tokens = Array.from($("msg-pickers").querySelectorAll("select")).map((s) => s.value);
    send("message.set", {
      tokens: tokens,
      dwell_s: parseInt($("msg-dwell").value, 10) || 0,
      hold: $("msg-hold").checked,
    });
  };

  $("btn-ramp").onclick = () => send("motion.ramp", {
    column: parseInt($("ramp-col").value, 10),
    from: parseInt($("ramp-from").value, 10),
    to: parseInt($("ramp-to").value, 10),
    step: parseInt($("ramp-step").value, 10),
    dwell_s: parseInt($("ramp-dwell").value, 10),
  });
  $("btn-ramp-stop").onclick = () => send("motion.ramp_stop");

  $("btn-spin").onclick = () => send("motion.spin", {
    column: parseInt($("spin-col").value, 10),
    flaps_s: parseInt($("spin-flaps").value, 10),
    seconds: parseInt($("spin-secs").value, 10),
  });

  // Sliders apply live; SAVE is separate.
  SLIDERS.forEach(([slider, out, key]) => {
    const node = $(slider);
    node.oninput = () => {
      $(out).textContent = node.value;
      const patch = {};
      patch[key] = parseInt(node.value, 10);
      send("motion.params", patch);
    };
  });
  $("set-enidle").onchange = () => send("motion.params", { en_idle_off: $("set-enidle").checked });
  $("btn-motion-save").onclick = () => send("motion.save");

  // Settings apply live too; SAVE persists.
  $("set-h24").onchange = () => send("clock.format", { h24: $("set-h24").checked });
  $("set-tz").onchange = () => pushConfig({ tz: $("set-tz").value.trim() });
  $("set-ntp").onchange = () => pushConfig({ ntp: $("set-ntp").value.trim() });
  // The list comes from the device (GET /api/wear enumerates exactly the
  // granularities it accepts), so a second copy of "the divisors of 60" cannot
  // drift from the one the API enforces.  The literals are only a fallback for
  // a page loaded while that fetch is failing.
  const gsel = $("set-gran");
  const fillGranularities = (list) => {
    if (gsel.childElementCount) return;
    list.forEach((g) => gsel.appendChild(el("option", { value: String(g) }, String(g))));
  };
  fetch("/api/wear").then((r) => r.json())
      .then((doc) => fillGranularities(doc.clock.map((e) => e.granularity_min)))
      .catch(() => fillGranularities([1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60]));
  gsel.onchange = () => pushConfig({ granularity_min: +gsel.value });
  $("set-live").onchange = () => pushConfig({ seconds_live_s: +$("set-live").value });
  $("set-secmode").onchange = () => pushConfig({ seconds_mode: $("set-secmode").value });
  $("set-zero").onchange = () => pushConfig({ zero_hold_s: +$("set-zero").value });
  $("set-spin").onchange = () => pushConfig({ spin_s: +$("set-spin").value });
  $("set-ftimeout").onchange = () => pushConfig({ failure_timeout_s: +$("set-ftimeout").value });
  $("set-cdland").onchange = () => pushConfig({ cd_land_on_tick: $("set-cdland").checked });
  $("set-clockland").onchange = () => pushConfig({ clock_land_on_tick: $("set-clockland").checked });
  $("set-dwell").onchange = () => pushConfig({ msg_dwell_s: +$("set-dwell").value });
  $("btn-config-save").onclick = () => send("config.save");

  // Per-column mode and maintenance, through the same dispatcher as everything
  // else.  "ALL SIM" is the one-click build-out setup; "ALL REAL" undoes it.
  $("btn-cols-real").onclick = () => send("motion.column", { all: true, mode: "real" });
  $("btn-cols-sim").onclick = () => send("motion.column", { all: true, mode: "sim" });
  $("set-maint").onchange = (e) => send("motion.maintenance", e.target.checked);

  // Audio.  oninput for the label, onchange for the command: a drag is one
  // setting, not two hundred.
  $("set-vol").oninput = () => { $("v-vol").textContent = $("set-vol").value; };
  $("set-vol").onchange = () => send("audio.volume", parseInt($("set-vol").value, 10));
  $("set-mute").onchange = () => send("audio.mute", $("set-mute").checked);

  const quietMinutes = (id) => {
    const v = $(id).value;
    if (!v) return 0;
    const [h, m] = v.split(":").map((x) => parseInt(x, 10));
    return h * 60 + m;
  };
  const pushQuiet = () => send("audio.quiet", {
    start_min: quietMinutes("set-quiet-start"),
    end_min: quietMinutes("set-quiet-end"),
  });
  $("set-quiet-start").onchange = pushQuiet;
  $("set-quiet-end").onchange = pushQuiet;
  $("btn-quiet-off").onclick = () => {
    $("set-quiet-start").value = "00:00";
    $("set-quiet-end").value = "00:00";
    pushQuiet();
  };

  // WiFi.  One command, same as the portal uses.
  $("btn-wifi-save").onclick = () => {
    const ssid = $("set-ssid").value.trim();
    if (!ssid) { toast("enter a network name", false); return; }
    send("wifi.credentials", { ssid, pass: $("set-wpass").value });
    $("set-wpass").value = "";
    toast("saved; joining " + ssid, true);
  };

  $("btn-portal").onclick = () => {
    const up = state && state.prov && state.prov.portal;
    send("wifi.provision", !up);
  };

  // MQTT.  Only the fields that carry something are sent: absent means KEEP,
  // which is the firmware's contract and the only way a form can save settings
  // it was never shown the password for.
  $("btn-mqtt-save").onclick = () => {
    const payload = { enabled: $("set-mqtt-en").checked };
    const put = (id, key) => {
      const v = $(id).value.trim();
      if (v !== "") payload[key] = v;
    };
    put("set-mqtt-uri", "uri");
    put("set-mqtt-base", "base");
    put("set-mqtt-hap", "ha_prefix");
    // The user IS sent even when empty, because the box shows what is stored:
    // clearing it has to mean clearing it.  The password is the opposite - the
    // box starts empty every time and cannot show what is stored, so an empty
    // one has to mean "leave it alone".
    payload.user = $("set-mqtt-user").value.trim();
    if ($("set-mqtt-pass").value !== "") payload.pass = $("set-mqtt-pass").value;
    bus.send("mqtt.config", payload);
    $("set-mqtt-pass").value = "";
    toast($("set-mqtt-en").checked ? "MQTT settings saved" : "MQTT off; discovery retracted", true);
  };

  $("btn-ota-upload").onclick = () => {
    const f = $("ota-file").files[0];
    if (!f) { toast("choose a firmware .bin first", false); return; }
    uploadFirmware(f, $("ota-force").checked);
  };

  // Not a data-cmd button: rebooting a pending image is how a rollback happens,
  // and that deserves a question rather than a click.
  $("btn-reboot").onclick = () => {
    if (state && state.sys && state.sys.ota_pending &&
        !window.confirm("This image has not confirmed itself yet. " +
                        "Rebooting now will roll the display back to the previous " +
                        "firmware. Reboot anyway?")) {
      return;
    }
    send("system.reboot");
  };

  $("btn-ring-upload").onclick = () => {
    const f = $("ring-file").files[0];
    if (!f) { toast("choose a ring.json first", false); return; }
    f.text().then((text) => fetch("/api/ring/upload", { method: "POST", body: text }))
      .then((r) => r.json())
      .then((r) => {
        if (r.ok) {
          toast("ring accepted - the modes task will swap it in", true);
          loadRing();
          if (state) loadWear(state.cfg, true);  // a new ring changes flip costs
        } else {
          toast("rejected: " + r.err, false);
        }
      })
      .catch(() => toast("upload failed", false));
  };
}

// --------------------------------------------------------------------------
function loadRing() {
  return fetch("/api/ring")
    .then((r) => r.json())
    .then((doc) => {
      ring = doc;
      if (flap) flap.setRing(ring);
      else flap = new SwanFlap.FlapDisplay($("display"), ring, { gapAfter: 2 });
      buildPickers();
      buildCal();
      if (state) renderSettings(state);
    });
}

// The glyph sheet is fetched once and injected, so every <use> is a
// same-document reference - external ones are unsupported in WebKit and would
// leave an iPhone staring at blank cards.  A failure just keeps the names.
SwanFlap.loadGlyphs("glyphs.svg").then((ok) => {
  if (ok && flap) flap.refresh();
});

// The controls and the socket do NOT depend on the ring document.  They used
// to: everything was chained off loadRing(), so one failed fetch - and that
// route is served by the single httpd task an OTA upload occupies for up to
// 120 s - left the panel with dead nav tabs, no WebSocket, and a toast that
// cleared itself after 2.6 s.  The presentation terminal already got this
// right, which is how the asymmetry was found.
wire();
bus.connect();
loadRing().catch(() => {
  toast("could not load /api/ring — pickers unavailable, controls still work", false);
});
