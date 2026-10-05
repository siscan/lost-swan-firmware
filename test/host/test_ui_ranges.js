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
// cannot go stale on its own: api.cpp and motion_types.h on one side, the <input>
// min/max in web/index.html on the other.  If a refactor moves a bound so that a
// pattern here stops matching, that is a failure too - a guard that silently finds
// nothing is worse than none.
"use strict";

const fs = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..", "..");
const read = (p) => fs.readFileSync(path.join(ROOT, p), "utf8");
const API = read("components/webapi/api.cpp");
const TYPES = read("components/motion/include/motion/motion_types.h");
const HTML = read("web/index.html");

let failures = 0;
function fail(what) { console.log("FAIL " + what); failures++; }

// ---- the firmware's side ----------------------------------------------------
function num(re, src, what) {
  const m = src.match(re);
  if (!m) { fail("could not find " + what + " in the firmware sources (pattern " + re + ")"); return null; }
  return m.slice(1).map(Number);
}
// `if (<var> < A || <var> > B) return err_result("<name> ...")`, the dispatcher's idiom
function fieldRange(name) {
  return num(new RegExp("v < (\\d+) \\|\\| v > (\\d+)\\) return err_result\\(\"" + name + " "), API, "the " + name + " range");
}

const speed = num(/want < (\d+) \|\| want > (\d+)\) return "out of range"/, API, "the speed range");
const spinFlaps = num(/flaps < (\d+) \|\| flaps > (\d+)\) return err_result\("flaps_s out of range"\)/, API, "the spin flaps/s range");
const spinSecs = num(/secs < (\d+) \|\| secs > (\d+)\) return err_result\("seconds out of range"\)/, API, "the spin seconds range");
const accelMin = num(/ACCEL_MIN\s*=\s*(\d+)\s*;/, TYPES, "ACCEL_MIN");
const accelMax = num(/ACCEL_MAX\s*=\s*(\d+)\s*;/, TYPES, "ACCEL_MAX");
const hallMin = num(/HALL_TOL_MIN\s*=\s*(\d+)\s*;/, TYPES, "HALL_TOL_MIN");
// HALL_TOL_MAX is DERIVED (half a flap), so read the value its own static_assert pins
const hallMax = num(/static_assert\(HALL_TOL_MAX == (\d+)/, TYPES, "HALL_TOL_MAX's pinned value");
const live = num(/seconds_live_s must be (\d+)\.\.(\d+)/, API, "the seconds_live_s range");
const volume = num(/v < (\d+) \|\| v > (\d+)\) return err_result\("volume must be/, API, "the volume range");

// What each control must agree with: [html id, [min, max], and the step if it has one]
const controls = [
  ["p-normal", speed], ["p-alarm", speed], ["p-home", speed],
  ["spin-flaps", spinFlaps], ["spin-secs", spinSecs],
  ["p-accel", accelMin && accelMax && [accelMin[0], accelMax[0]]],
  ["p-halltol", hallMin && hallMax && [hallMin[0], hallMax[0]]],
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
if (hallMax && hallMax[0] !== 32) fail("HALL_TOL_MAX is half a flap, 32, at the 1:1 drive (got " + hallMax[0] + ")");
if (accelMax && accelMax[0] >= 82000) fail("ACCEL_MAX must stay below the 82000 that stalled the drum (2026-09-12)");

if (failures) {
  console.log(failures + " failure(s)");
  process.exit(1);
}
console.log("all checks passed");
