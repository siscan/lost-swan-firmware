// The countdown rendering contract, in JavaScript (spec §7.3).
//
// `web/terminal.js`'s countdownShownS is a deliberate PORT of the firmware's
// countdown_shown_s, because the presentation readout, the separate terminal
// prop and the drums all derive their display from one deadline and must round
// the same way.  A port that drifts is two screens disagreeing about the same
// countdown by a whole step, which is exactly what nobody would think to check.
//
// So this suite asserts the same properties test_modes.cpp asserts, against the
// JavaScript, and the two lists are meant to be read side by side.
//
// No npm: terminal.js is a browser file, so the two functions are lifted out of
// it by source extraction rather than imported.  If that extraction fails, the
// suite says so and fails - it does not quietly pass.
"use strict";

const fs = require("fs");
const path = require("path");

const SRC = path.join(__dirname, "..", "..", "web", "terminal.js");

let failures = 0;
function check(cond, what) {
  if (!cond) {
    console.log("FAIL " + what);
    ++failures;
  }
}
function eq(got, want, what) {
  if (got !== want) {
    console.log("FAIL " + what + "  (" + got + " vs " + want + ")");
    ++failures;
  }
}

// ---------------------------------------------------------------------------
// Lift the two functions out of the browser file.
// ---------------------------------------------------------------------------
function extract(src, name) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) return null;
  let depth = 0;
  let i = src.indexOf("{", at);
  const start = at;
  for (; i < src.length; i++) {
    if (src[i] === "{") depth++;
    else if (src[i] === "}") {
      depth--;
      if (depth === 0) return src.slice(start, i + 1);
    }
  }
  return null;
}

const src = fs.readFileSync(SRC, "utf8");
const stepSrc = extract(src, "countdownStepS");
const shownSrc = extract(src, "countdownShownS");
if (!stepSrc || !shownSrc) {
  console.log("FAIL could not extract countdownStepS/countdownShownS from web/terminal.js");
  console.log("     the contract is unverified - fix the extraction rather than deleting this");
  process.exit(1);
}
// eslint-disable-next-line no-new-func
const mod = new Function(stepSrc + "\n" + shownSrc +
                         "\nreturn {countdownStepS, countdownShownS};")();
const stepS = mod.countdownStepS;
const shownS = mod.countdownShownS;

const LIVE = 240;

// ---------------------------------------------------------------------------
// (1) 108:00 is held for the first full minute.
// ---------------------------------------------------------------------------
eq(shownS("seconds", 6480, LIVE), 6480, "start face is 108:00");
eq(shownS("seconds", 6479, LIVE), 6480, "still 108:00 half a second in");
eq(shownS("seconds", 6421, LIVE), 6480, "still 108:00 at 6421");
eq(shownS("seconds", 6420, LIVE), 6420, "107:00 only at 6420");

// ---------------------------------------------------------------------------
// (2) The 4:00 transition is seamless.
// ---------------------------------------------------------------------------
eq(shownS("seconds", 300, LIVE), 300, "005:00 at 300");
eq(shownS("seconds", 241, LIVE), 300, "005:00 owns the minute above the boundary");
eq(shownS("seconds", 240, LIVE), 240, "004:00 exactly at the boundary");
eq(shownS("seconds", 239, LIVE), 239, "then one second at a time");

// ---------------------------------------------------------------------------
// (3) 000:00 lands at zero, not a second early.
// ---------------------------------------------------------------------------
eq(shownS("seconds", 1, LIVE), 1, "000:01 while a second remains");
eq(shownS("seconds", 0, LIVE), 0, "000:00 exactly at zero");

// ---------------------------------------------------------------------------
// The modes, and the step rule.
// ---------------------------------------------------------------------------
eq(stepS("minutes", 10, LIVE), 60, "minutes mode never shows seconds");
eq(stepS("tens", 240, LIVE), 10, "tens mode steps by ten inside the window");
eq(stepS("seconds", 240, LIVE), 1, "seconds mode steps by one inside the window");
eq(stepS("seconds", 241, LIVE), 60, "and by a minute outside it");
eq(shownS("tens", 239, LIVE), 240, "tens: 239 still ceilings to 240");
eq(shownS("tens", 230, LIVE), 230, "tens: 230 is its own window");
eq(shownS("minutes", 55, LIVE), 60, "minutes: 001:00 while 55 s remain, never 000:00");

// ---------------------------------------------------------------------------
// The three invariants, swept - the same sweep test_modes.cpp runs.
// ---------------------------------------------------------------------------
for (const mode of ["minutes", "tens", "seconds"]) {
  for (const live of [0, 1, 30, 59, 60, 61, 100, 239, 240, 241, 250, 299, 600, 6480]) {
    let prev = -1;
    for (let r = 0; r <= 6480; r++) {
      const shown = shownS(mode, r, live);
      const step = stepS(mode, r, live);
      if (shown < prev) {
        check(false, "non-monotonic: mode=" + mode + " live=" + live + " r=" + r);
        break;
      }
      if (shown < r) {
        check(false, "shown BEHIND remaining: mode=" + mode + " live=" + live +
                     " r=" + r + " shown=" + shown);
        break;
      }
      if (r > 0 && (shown - r >= step || shown % step !== 0)) {
        check(false, "bad window: mode=" + mode + " live=" + live + " r=" + r +
                     " shown=" + shown + " step=" + step);
        break;
      }
      prev = shown;
    }
  }
}

// ---------------------------------------------------------------------------
// And the flood cadence, which is the other half of the §7.3 contract.
// ---------------------------------------------------------------------------
const proto = fs.readFileSync(path.join(__dirname, "..", "..", "web", "protocol.js"), "utf8");
check(proto.indexOf('const SYSTEM_FAILURE = "SYSTEM FAILURE";') >= 0,
      "protocol.js carries the exact 14-character literal");
check(proto.indexOf("const REPEAT_MS = 100;") >= 0,
      "protocol.js repeats every 100 ms, per the cadence contract");
check(proto.indexOf('flood += SYSTEM_FAILURE;') >= 0,
      "protocol.js appends with NO separator and NO newline");
check(!/flood \+= SYSTEM_FAILURE \+ ["'\\ ]/.test(proto),
      "protocol.js does not append a separator");
eq("SYSTEM FAILURE".length, 14, "the literal is fourteen characters");

if (failures) {
  console.log(failures + " failure(s)");
  process.exit(1);
}
console.log("all checks passed");
