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
let sock = null;
let nextId = 1;
let cfgDirty = false;  // a settings field is focused: do not overwrite it

// --------------------------------------------------------------------------
// Transport
// --------------------------------------------------------------------------
function toast(msg, ok) {
  const t = $("toast");
  t.textContent = msg;
  t.className = "show " + (ok ? "ok" : "err");
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { t.className = ""; }, 2600);
}

function send(cmd, payload) {
  const body = { cmd: cmd, id: nextId++ };
  if (payload !== undefined) body.payload = payload;
  const text = JSON.stringify(body);
  if (sock && sock.readyState === 1) {
    sock.send(text);
    return;
  }
  fetch("/api/cmd", { method: "POST", body: text })
    .then((r) => r.json())
    .then((r) => { if (!r.ok) toast(cmd + ": " + (r.err || "rejected"), false); })
    .catch(() => toast("not connected", false));
}

function connect() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  sock = new WebSocket(proto + "//" + location.host + "/ws");
  sock.onopen = () => {
    $("link").textContent = "connected";
    $("link").className = "ok";
  };
  sock.onclose = () => {
    $("link").textContent = "disconnected";
    $("link").className = "bad";
    sock = null;
    setTimeout(connect, 1500);
  };
  sock.onerror = () => { if (sock) sock.close(); };
  sock.onmessage = (m) => {
    let e;
    try { e = JSON.parse(m.data); } catch (_) { return; }
    switch (e.e) {
      case "state":  onState(e); break;
      case "go":     flap.flipTo(e.col, e.idx, e.flaps); break;
      case "spin":   flap.spin(e.col, e.flaps, e.secs); break;
      case "mode":   $("s-mode").textContent = e.name; break;
      case "cue":    toast("♪ " + e.name, true); break;
      case "result":
        if (e.res && e.res.ok === false) toast(e.res.err || "rejected", false);
        break;
      default: break;
    }
  };
}

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
  // animating fifty phantom flips.
  if (flap && !flap.primed) {
    flap.setAll(s.cols.map((c) => c.index));
    flap.primed = true;
  }

  renderDiag(s);
  renderCal(s);
  if (!cfgDirty) renderSettings(s);

  $("ramp-state").textContent = s.cal.ramp_active
      ? "walking column " + (s.cal.ramp_col + 1)
      : "";
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
  $("set-zero").value = s.cfg.zero_hold_s;
  $("set-spin").value = s.cfg.spin_s;
  $("set-ftimeout").value = s.cfg.failure_timeout_s;
  $("set-cdland").checked = s.cfg.cd_land_on_tick;
  $("set-clockland").checked = s.cfg.clock_land_on_tick;
  $("set-dwell").value = s.cfg.msg_dwell_s;
  $("gran-hint").textContent = wearHint(s.cfg.granularity_min);

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
}

// The measured table from spec §7.1, so the cost of a change is visible where
// the change is made.
function wearHint(min) {
  const known = { 1: "≈39,500", 5: "≈10,700", 15: "≈5,900", 30: "≈2,300", 60: "≈1,100" };
  return known[min] ? known[min] + " flips/day" : "";
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
  $("set-gran").onchange = () => pushConfig({ granularity_min: +$("set-gran").value });
  $("set-secmode").onchange = () => pushConfig({ seconds_mode: $("set-secmode").value });
  $("set-zero").onchange = () => pushConfig({ zero_hold_s: +$("set-zero").value });
  $("set-spin").onchange = () => pushConfig({ spin_s: +$("set-spin").value });
  $("set-ftimeout").onchange = () => pushConfig({ failure_timeout_s: +$("set-ftimeout").value });
  $("set-cdland").onchange = () => pushConfig({ cd_land_on_tick: $("set-cdland").checked });
  $("set-clockland").onchange = () => pushConfig({ clock_land_on_tick: $("set-clockland").checked });
  $("set-dwell").onchange = () => pushConfig({ msg_dwell_s: +$("set-dwell").value });
  $("btn-config-save").onclick = () => send("config.save");

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

loadRing().then(() => {
  wire();
  connect();
}).catch(() => {
  toast("could not load /api/ring", false);
});
