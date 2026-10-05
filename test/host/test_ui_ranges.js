// The Settings and Calibrate controls against the firmware's own bounds.
//
// The rule written next to motion.accel is that the load path, the dispatcher and
// the web slider "cannot drift apart".  That is a promise a test has to keep,
// because the failure is invisible and it happened to hall_tol: the slider ran
// 1..400, the dispatcher accepted 1..400, and config::load discarded anything over
// 32 - so dragging the slider to the right produced a tolerance that blinded slip
// detection (slips of up to 4.7 flaps raised no fault, probed on the real core)
// and the next boot quietly put it back to 16.
//
// This reads the bounds OUT OF THE SOURCES rather than restating them, so it
// cannot go stale on its own: limits_policy.h, motion_types.h and api.cpp on one
// side, the <input> min/max in web/index.html on the other.  If a refactor moves a
// bound so that a pattern here stops matching, that is a failure too - a guard that
// silently finds nothing is worse than none.
//
// THE UNLIMITED FLAVOUR (motion/limits_policy.h) is why this reads the NORMAL
// bounds: index.html is one file for every image, so its control bounds are the
// normal image's, and an unlimited image overrides them at runtime from
// `motion.ranges` in the state document (app.js applyRanges).  The second half of
// this file runs that function, as shipped, against both kinds of document.
"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const ROOT = path.join(__dirname, "..", "..");
const read = (p) => fs.readFileSync(path.join(ROOT, p), "utf8");
const API = read("components/webapi/api.cpp");
const TYPES = read("components/motion/include/motion/motion_types.h");
const LIMITS = read("components/motion/include/motion/limits_policy.h");
const HTML = read("web/index.html");
const APPJS = read("web/app.js");

let failures = 0;
function fail(what) { console.log("FAIL " + what); failures++; }

// ---- the firmware's side ----------------------------------------------------
function num(re, src, what) {
  const m = src.match(re);
  if (!m) { fail("could not find " + what + " in the firmware sources (pattern " + re + ")"); return null; }
  return m.slice(1).map(Number);
}
// `if (v < A || v > B) return err_result("<name> ...")`, the dispatcher's idiom
function fieldRange(name) {
  return num(new RegExp("v < (\\d+) \\|\\| v > (\\d+)\\) return err_result\\(\"" + name + " "), API, "the " + name + " range");
}
function has(re, src, what) {
  if (!re.test(src)) fail("the sources no longer " + what + " (pattern " + re + ")");
}

// The NORMAL image's bounds, which are the literals index.html must match.
const flapsMin = num(/FLAPS_S_MIN\s*=\s*(\d+)\s*;/, LIMITS, "FLAPS_S_MIN");
const flapsMaxNormal = num(/FLAPS_S_MAX_NORMAL\s*=\s*(\d+)\s*;/, LIMITS, "FLAPS_S_MAX_NORMAL");
const flapsMaxUnl = num(/FLAPS_S_MAX_UNLIMITED\s*=\s*SHOW_SPIN_FLAPS_S\s*;/, LIMITS, "FLAPS_S_MAX_UNLIMITED = SHOW_SPIN_FLAPS_S") && [400];
const speed = flapsMin && flapsMaxNormal && [flapsMin[0], flapsMaxNormal[0]];
const spinSecs = num(/secs < (\d+) \|\| secs > (\d+)\) return err_result\("seconds out of range"\)/, API, "the spin seconds range");
const accelMin = num(/ACCEL_MIN\s*=\s*(\d+)\s*;/, TYPES, "ACCEL_MIN");
const accelMaxNormal = num(/ACCEL_MAX_NORMAL\s*=\s*(\d+)\s*;/, LIMITS, "ACCEL_MAX_NORMAL");
const accelMaxUnl = num(/ACCEL_MAX_UNLIMITED\s*=\s*(\d+)\s*;/, LIMITS, "ACCEL_MAX_UNLIMITED");
const hallMin = num(/HALL_TOL_MIN\s*=\s*(\d+)\s*;/, TYPES, "HALL_TOL_MIN");
// HALL_TOL_MAX_* are DERIVED from the flap, so read the value each static_assert pins
const hallMaxNormal = num(/static_assert\(HALL_TOL_MAX_NORMAL == (\d+)/, LIMITS, "HALL_TOL_MAX_NORMAL's pinned value");
const hallMaxUnl = num(/static_assert\(HALL_TOL_MAX_UNLIMITED == (\d+)/, LIMITS, "HALL_TOL_MAX_UNLIMITED's pinned value");
const live = num(/seconds_live_s must be (\d+)\.\.(\d+)/, API, "the seconds_live_s range");
const volume = num(/v < (\d+) \|\| v > (\d+)\) return err_result\("volume must be/, API, "the volume range");

// The dispatcher must be built from THIS IMAGE's range, not from literals - a
// literal here is how the sliders and the firmware drift apart.  Three sites.
has(/if \(!motion::flaps_s_plausible\(want\)\) return "out of range \(" \+ speed_range_text\(\)/, API,
    "judge motion.params speeds by motion::flaps_s_plausible");
has(/if \(!motion::flaps_s_plausible\(flaps\)\) \{\s*return err_result\("flaps_s out of range \(" \+ speed_range_text\(\)/, API,
    "judge motion.spin by motion::flaps_s_plausible");
has(/if \(!accel_plausible\(static_cast<int32_t>\(v\)\)\)/, API, "judge accel by accel_plausible");
has(/if \(!hall_tol_plausible\(static_cast<int32_t>\(v\)\)\)/, API, "judge hall_tol by hall_tol_plausible");
// ... and the image-wide constants must be the normal literal in a normal image.
has(/FLAPS_S_MAX\s*=\s*UNLIMITED_BUILD\s*\?\s*FLAPS_S_MAX_UNLIMITED\s*:\s*FLAPS_S_MAX_NORMAL\s*;/, LIMITS,
    "define FLAPS_S_MAX as the normal literal unless UNLIMITED_BUILD");
has(/ACCEL_MAX\s*=\s*motion::UNLIMITED_BUILD \? motion::ACCEL_MAX_UNLIMITED : motion::ACCEL_MAX_NORMAL\s*;/, TYPES,
    "define ACCEL_MAX as the normal literal unless UNLIMITED_BUILD");
has(/HALL_TOL_MAX =\s*motion::UNLIMITED_BUILD \? motion::HALL_TOL_MAX_UNLIMITED : motion::HALL_TOL_MAX_NORMAL\s*;/, TYPES,
    "define HALL_TOL_MAX as the normal value unless UNLIMITED_BUILD");

// What each control must agree with: [html id, [min, max], and the step if it has one]
const controls = [
  ["p-normal", speed], ["p-alarm", speed], ["p-home", speed],
  ["spin-flaps", speed], ["spin-secs", spinSecs],
  ["p-accel", accelMin && accelMaxNormal && [accelMin[0], accelMaxNormal[0]]],
  ["p-halltol", hallMin && hallMaxNormal && [hallMin[0], hallMaxNormal[0]]],
  ["msg-dwell", fieldRange("msg_dwell_s")], ["set-dwell", fieldRange("msg_dwell_s")],
  ["set-zero", fieldRange("zero_hold_s")], ["set-spin", fieldRange("spin_s")],
  ["set-floop", fieldRange("failure_loop_s")], ["set-ftimeout", fieldRange("failure_timeout_s")],
  ["set-live", live && [live[0], live[1]]], ["set-vol", volume],
];

// ---- the page's side --------------------------------------------------------
function attrs(id) {
  const m = HTML.match(new RegExp("<input\\b[^>]*\\bid=\"" + id + "\"[^>]*>"));
  if (!m) return null;
  const get = (k) => { const a = m[0].match(new RegExp("\\b" + k + "=\"(-?\\d+)\"")); return a ? Number(a[1]) : null; };
  return { min: get("min"), max: get("max") };
}

for (const [id, fw] of controls) {
  if (!fw) continue;                       // the extractor already said why
  const ui = attrs(id);
  if (!ui) { fail("web/index.html has no <input id=\"" + id + "\">"); continue; }
  if (ui.min !== fw[0] || ui.max !== fw[1]) {
    fail(id + ": the control runs " + ui.min + ".." + ui.max + " but the firmware accepts " + fw[0] + ".." + fw[1]);
  }
}

// The shared-bound promises that have a second home, said once more where they live
if (hallMaxNormal && hallMaxNormal[0] !== 32) fail("HALL_TOL_MAX_NORMAL is half a flap, 32, at the 1:1 drive (got " + hallMaxNormal[0] + ")");
if (hallMaxUnl && hallMaxUnl[0] !== 64) fail("HALL_TOL_MAX_UNLIMITED is one flap, 64, at the 1:1 drive (got " + hallMaxUnl[0] + ")");
if (accelMaxNormal && accelMaxNormal[0] >= 82000) fail("ACCEL_MAX_NORMAL must stay below the 82000 that stalled the drum (2026-09-12)");
if (flapsMaxNormal && flapsMaxUnl && flapsMaxUnl[0] <= flapsMaxNormal[0]) fail("the unlimited flavour must widen the speed range");

// ---- THE UNLIMITED IMAGE: the page follows the firmware ---------------------
// Run applyRanges() exactly as shipped (the code between the markers in app.js)
// against a fake page built from index.html's own control bounds.
const block = APPJS.match(/\/\/ <ranges>\n([\s\S]*?)\/\/ <\/ranges>/);
if (!block) {
  fail("web/app.js has no // <ranges> ... // </ranges> block to run");
} else {
  const ids = ["p-normal", "p-alarm", "p-home", "spin-flaps", "p-accel", "p-halltol"];
  const nodes = {};
  const defaults = {};
  for (const id of ids) {
    const a = attrs(id);
    if (!a) { fail("web/index.html has no <input id=\"" + id + "\">"); continue; }
    defaults[id] = { min: String(a.min), max: String(a.max) };
    const at = { min: String(a.min), max: String(a.max) };
    nodes[id] = { dataset: {}, getAttribute: (k) => at[k], setAttribute: (k, v) => { at[k] = String(v); }, at };
  }
  nodes["unlimited-note"] = { style: { display: "none" }, textContent: "" };
  // The hall_tol hint's "where it stops" phrase, whose text index.html carries for a normal image.
  const stopMatch = HTML.match(/<span id="halltol-stop">([\s\S]*?)<\/span>/);
  if (!stopMatch) fail("web/index.html has no <span id=\"halltol-stop\"> in the hall_tol hint");
  const stopText0 = stopMatch ? stopMatch[1] : "";
  nodes["halltol-stop"] = { dataset: {}, textContent: stopText0 };
  const ctx = vm.createContext({ $: (id) => nodes[id] || null });
  vm.runInContext(block[1] + "\nthis.applyRanges = applyRanges;", ctx, { filename: "app.js <ranges>" });
  const apply = ctx.applyRanges;

  const bounds = () => Object.fromEntries(ids.map((id) => [id, nodes[id].at.min + ".." + nodes[id].at.max]));
  const expectDefaults = (why) => {
    for (const id of ids) {
      const want = defaults[id].min + ".." + defaults[id].max;
      if (bounds()[id] !== want) fail(id + " " + why + ": " + bounds()[id] + " but index.html says " + want);
    }
    if (nodes["unlimited-note"].style.display !== "none") fail("the unlimited note is showing " + why);
    if (nodes["halltol-stop"].textContent !== stopText0) {
      fail("the hall_tol hint says \"" + nodes["halltol-stop"].textContent + "\" " + why + ", index.html says \"" + stopText0 + "\"");
    }
  };

  // 1. A normal image publishes unlimited:false and NO ranges: the markup is the truth.
  apply({ motion: { unlimited: false } });
  expectDefaults("on a normal image");
  apply({});                                   // an image too old to publish `motion` at all
  expectDefaults("on a document with no motion block");

  // 2. An unlimited image: every control follows the document, nothing else moves.
  const ranges = {
    flaps_s_min: flapsMin[0], flaps_s_max: flapsMaxUnl[0],
    accel_min: accelMin[0], accel_max: accelMaxUnl[0],
    hall_tol_min: hallMin[0], hall_tol_max: hallMaxUnl[0],
    persist_flaps_s_max: flapsMaxNormal[0], persist_accel_max: accelMaxNormal[0],
    persist_hall_tol_max: hallMaxNormal[0],
  };
  apply({ motion: { unlimited: true, ranges } });
  const b = bounds();
  for (const id of ["p-normal", "p-alarm", "p-home", "spin-flaps"]) {
    if (b[id] !== flapsMin[0] + ".." + flapsMaxUnl[0]) fail(id + " did not follow the unlimited speed range: " + b[id]);
  }
  if (b["p-accel"] !== accelMin[0] + ".." + accelMaxUnl[0]) fail("p-accel did not follow the unlimited accel range: " + b["p-accel"]);
  if (b["p-halltol"] !== hallMin[0] + ".." + hallMaxUnl[0]) fail("p-halltol did not follow the unlimited hall_tol range: " + b["p-halltol"]);
  // The hall_tol hint must not claim half a flap on an image whose slider runs to a whole one.
  if (!/one flap \(64\)/.test(nodes["halltol-stop"].textContent)) {
    fail("the hall_tol hint does not say where an unlimited image's slider stops: \"" + nodes["halltol-stop"].textContent + "\"");
  }
  const note = nodes["unlimited-note"];
  if (note.style.display === "none") fail("the unlimited note is hidden on an unlimited image");
  for (const need of [String(flapsMaxUnl[0]), String(accelMaxNormal[0]), "NOT saved", "ramp-power guard"]) {
    if (!note.textContent.includes(need)) fail("the unlimited note does not mention " + need);
  }

  // 3. THE BOARD IS REFLASHED UNDER AN OPEN PAGE: a normal document arrives and the
  //    controls must go back.  A slider left running to 400 on a normal image is
  //    exactly the hall_tol bug again.
  apply({ motion: { unlimited: false } });
  expectDefaults("after the image stopped being unlimited");

  // 4. A document that CLAIMS unlimited and carries no ranges must not leave the
  //    controls half-widened or the note up with nothing to say.
  apply({ motion: { unlimited: true, ranges } });
  apply({ motion: { unlimited: true } });
  expectDefaults("when an unlimited document carries no ranges");

  // 5. Nothing else on the page is touched: a control the function does not know
  //    about is simply not asked for.
  const asked = [];
  const ctx2 = vm.createContext({ $: (id) => { asked.push(id); return nodes[id] || null; } });
  vm.runInContext(block[1] + "\nthis.applyRanges = applyRanges;", ctx2);
  ctx2.applyRanges({ motion: { unlimited: true, ranges } });
  const known = new Set(ids.concat(["unlimited-note", "halltol-stop"]));
  for (const id of asked) if (!known.has(id)) fail("applyRanges reached for an unexpected element: " + id);
  for (const id of ids) if (!asked.includes(id)) fail("applyRanges never reached " + id);
}

// ---- the document, the page and the firmware agree on the keys --------------
// The page reads `motion.ranges.<base>_min/_max` and the three `persist_*_max`
// keys; the firmware must be the one publishing them, under those names.
for (const key of ["flaps_s_min", "flaps_s_max", "accel_min", "accel_max", "hall_tol_min",
                   "hall_tol_max", "persist_flaps_s_max", "persist_accel_max", "persist_hall_tol_max"]) {
  if (!API.includes(".kv(\"" + key + "\"")) fail("api.cpp does not publish motion.ranges." + key);
  if (!APPJS.includes(key) && !APPJS.includes(key.replace(/_(min|max)$/, ""))) {
    fail("app.js never reads motion.ranges." + key);
  }
}
if (!/id="unlimited-note"[^>]*style="display:none"/.test(HTML)) {
  fail("index.html's #unlimited-note must exist and START hidden: a normal image shows nothing");
}
// The strip and the chip: the two surfaces a person sees without opening anything.
// The strip's function BODY only - not "the rest of the file", which would let an
// unrelated m.unlimited further down satisfy the check.
const rigStart = APPJS.indexOf("function renderRig");
const rigEnd = APPJS.indexOf("\nfunction ", rigStart + 1);
if (rigStart < 0 || rigEnd < 0) fail("could not find renderRig in web/app.js");
else if (!/if \(m\.unlimited\) \{[\s\S]*?UNLIMITED IMAGE/.test(APPJS.slice(rigStart, rigEnd))) fail("the control panel's rig strip does not announce an unlimited image");
if (!/m\.unlimited\) rigBits\.push\("UNLIMITED"\)/.test(read("web/terminal.js"))) fail("the presentation header's chip does not announce an unlimited image");

if (failures) {
  console.log(failures + " failure(s)");
  process.exit(1);
}
console.log("all checks passed");
