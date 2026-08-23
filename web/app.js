// LOST Swan split-flap - web UI (spec 10.2).
//
// Every control on every page sends a §10.2a command through /ws (or POST
// /api/cmd when the socket is down).  There is no second control path: what
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
let cfgDirty = false;  // a settings field is focused: do not overwrite it
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
  },
});

const send = (cmd, payload) => bus.send(cmd, payload);

bus.on("state", (e) => onState(e))
   .on("go", (e) => flap && flap.flipTo(e.col, e.idx, e.flaps))
   .on("spin", (e) => flap && flap.spin(e.col, e.flaps, e.secs))
   .on("mode", (e) => { $("s-mode").textContent = e.name; })
   .on("cue", (e) => toast("♪ " + e.name, true))
   .on("result", (e) => {
     if (e.res && e.res.ok === false) toast(e.res.err || "rejected", false);
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

  // The mirror follows the axes, so a page opened mid-run catches up without
  // animating fifty phantom flips.  A column with index -1 paints as UNKNOWN,
  // not as the blank flap - they are different facts.
  if (flap && !flap.primed) {
    flap.setAll(s.cols.map((c) => c.index));
    flap.primed = true;
  }
  if (flap) {
    flap.setStates(s.cols);
    // An axis that has gone unknown while the page was open must stop showing
    // a stale face; the go/spin events cannot express "I no longer know".
    s.cols.forEach((c, i) => {
      if (c.index < 0 && flap.cols[i] && flap.cols[i].idx >= 0 && !flap.cols[i].timer) {
        flap.paintUnknown(i);
      }
    });
  }
  renderMotion(s);

  renderDiag(s);
  renderCal(s);
  if (!cfgDirty) renderSettings(s);

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
      return n + (c.retry > 0 ? " re-homing " + c.retry + "/3" : " homing");
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
      [c.retry > 0 ? c.retry + "/3" : "—", false],
    ];
    cells.forEach(([text, num]) => tr.appendChild(el("td", num ? { class: "num" } : null, text)));
    body.appendChild(tr);
  });

  const sys = $("diag-sys");
  sys.textContent = "";
  const facts = [
    ["wifi", s.sys.wifi + (s.sys.ssid ? " · " + s.sys.ssid : "")],
    ["address", (s.sys.ip || "—") + " · " + s.sys.host + ".local"],
    ["rssi", s.sys.rssi + " dBm"],
    ["heap", s.sys.heap + " B"],
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
];

function renderSettings(s) {
  $("set-h24").checked = s.cfg.h24;
  if (document.activeElement !== $("set-tz")) $("set-tz").value = s.cfg.tz;
  if (document.activeElement !== $("set-gran")) $("set-gran").value = s.cfg.granularity_min;
  $("set-secmode").value = s.cfg.seconds_mode;
  if (document.activeElement !== $("set-live")) $("set-live").value = s.cfg.seconds_live_s;
  $("set-zero").value = s.cfg.zero_hold_s;
  $("set-spin").value = s.cfg.spin_s;
  $("set-ftimeout").value = s.cfg.failure_timeout_s;
  $("set-cdland").checked = s.cfg.cd_land_on_tick;
  $("set-clockland").checked = s.cfg.clock_land_on_tick;
  $("set-dwell").value = s.cfg.msg_dwell_s;
  loadWear(s.cfg, false).then(() => renderWear(s.cfg));

  SLIDERS.forEach(([slider, out, key]) => {
    const node = $(slider);
    if (document.activeElement !== node) node.value = s.cfg[key];
    $(out).textContent = node.value;
  });

  const sel = document.querySelectorAll(".reveal-sel");
  s.cfg.reveal.forEach((name, i) => {
    if (sel[i] && document.activeElement !== sel[i]) sel[i].value = name === null ? "" : name;
  });

  $("ring-info").textContent = "loaded from " + s.ring.source + " · " +
      s.ring.slots + " slots per drum";

  renderColumnModes(s);
  $("set-maint").checked = !!(s.motion && s.motion.maintenance);
  $("maint-hint").textContent = s.motion && s.motion.maintenance
      ? "suspended — nothing is scheduled and nothing re-homes"
      : "";
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
    if (document.activeElement !== sel) sel.value = c.mode;
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

  $("btn-execute").onclick = () => send("countdown.execute", $("numbers").value.trim());
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
  $("btn-motion-save").onclick = () => send("motion.save");

  // Settings apply live too; SAVE persists.
  $("set-h24").onchange = () => send("clock.format", { h24: $("set-h24").checked });
  $("set-tz").onchange = () => pushConfig({ tz: $("set-tz").value.trim() });
  const GRANULARITIES = [1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60];
  const gsel = $("set-gran");
  GRANULARITIES.forEach((g) => gsel.appendChild(el("option", { value: String(g) }, String(g))));
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

  // Do not clobber a field while it is being typed into.
  document.querySelectorAll("#page-settings input, #page-settings select").forEach((n) => {
    n.addEventListener("focus", () => { cfgDirty = true; });
    n.addEventListener("blur", () => { cfgDirty = false; });
  });

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

loadRing().then(() => {
  wire();
  bus.connect();
}).catch(() => {
  toast("could not load /api/ring", false);
});
