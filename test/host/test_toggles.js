// The toggle matrix (rule 1, 2026-08-25).
//
// CONTENT and PRESENTATION are orthogonal.  Station and protocol say WHAT is on
// screen; CRT, key click and mirror say how it looks.  Every combination must
// compose, no toggle may alter another toggle's state, and each must persist
// independently.
//
// This is a test rather than a checklist because the failure it exists for was
// invisible from inside any one setting: turning protocol mode ON silently lost
// the CRT, and you only ever see that if you happened to have had CRT on first.
// Walking the matrix is the only way to catch a toggle that writes another one.
//
// There is no browser and no npm here, so the handful of DOM and storage calls
// the pref layer touches are faked, and the REAL loadPrefs / savePref /
// applyPrefs / setStation are lifted out of web/terminal.js by source
// extraction - testing a copy of the logic would defeat the point.  If the
// extraction stops matching, the suite FAILS rather than quietly passing.
"use strict";

const fs = require("fs");
const path = require("path");

const SRC = path.join(__dirname, "..", "..", "web", "terminal.js");
const src = fs.readFileSync(SRC, "utf8");

let failures = 0;
function fail(what) { console.log("FAIL " + what); ++failures; }
function eq(got, want, what) {
  if (got !== want) fail(what + "  (got " + got + ", want " + want + ")");
}

// ---------------------------------------------------------------------------
// Lift the real pref layer out of terminal.js.
// ---------------------------------------------------------------------------
function extractFn(name) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) return null;
  let depth = 0;
  for (let i = src.indexOf("{", at); i < src.length; i++) {
    if (src[i] === "{") depth++;
    else if (src[i] === "}" && --depth === 0) return src.slice(at, i + 1);
  }
  return null;
}

const NAMES = ["loadPrefs", "savePref", "applyPrefs", "setStation"];
const bodies = NAMES.map(extractFn);
const prefsAt = src.indexOf("const PREFS = {");
const prefsBlock = prefsAt < 0 ? null
    : src.slice(prefsAt, src.indexOf("};", prefsAt) + 2);
const missing = NAMES.filter((n, i) => !bodies[i]);
if (!prefsBlock || missing.length) {
  console.log("FAIL could not extract from web/terminal.js: " +
              (missing.join(", ") || "the PREFS block"));
  console.log("     the toggle matrix is UNVERIFIED - fix the extraction here,");
  console.log("     do not delete the assertion");
  process.exit(1);
}

const PRESENTATION = ["crt", "click", "dock"];
const CONTENT = ["protocol", "egg"];
const ALL = PRESENTATION.concat(CONTENT);
const STATIONS = ["swan", "pearl", "flame"];

// ---------------------------------------------------------------------------
// A DOM and a localStorage, just big enough, and a fresh module over them.
// Each instance is a browser profile: `store` is its localStorage, so an empty
// one is a clean profile and a populated one is a return visit.
// ---------------------------------------------------------------------------
function makeEl(dataset) {
  const cls = new Set();
  return {
    dataset: dataset || {},
    style: {},
    classList: {
      toggle(n, on) { if (on === undefined) on = !cls.has(n); on ? cls.add(n) : cls.delete(n); },
      add(n) { cls.add(n); },
      remove(n) { cls.delete(n); },
      contains(n) { return cls.has(n); },
    },
  };
}

function makeModule(store) {
  store = store || {};
  const nodes = {
    screen: makeEl(), dock: makeEl(), foot: makeEl(), html: makeEl(),
    toggles: ALL.map((t) => makeEl({ toggle: t })),
    stations: STATIONS.map((s) => makeEl({ station: s })),
  };
  global.localStorage = {
    getItem: (k) => (k in store ? store[k] : null),
    setItem: (k, v) => { store[k] = String(v); },
    removeItem: (k) => { delete store[k]; },
  };
  global.document = {
    documentElement: nodes.html,
    getElementById: (id) => nodes[id] || null,
    querySelectorAll: (sel) => (sel.indexOf("data-toggle") >= 0 ? nodes.toggles
                              : sel.indexOf("data-station") >= 0 ? nodes.stations
                              : []),
  };
  global.window = {};        // applyPrefs probes window.SwanProtocol / SwanTerm

  const shim = [
    prefsBlock,
    'const STATION_KEY = "swan.term.station";',
    'let stationName = "swan";',
    "const $ = (id) => document.getElementById(id);",
    "const SwanTerm = { emit() {} };",
    bodies.join("\n"),
    "return { PREFS, loadPrefs, savePref, applyPrefs, setStation,",
    "         station: () => stationName, nodes, store };",
  ].join("\n");
  // eslint-disable-next-line no-new-func
  const M = new Function("nodes", "store", shim)(nodes, store);
  // What the strip's buttons actually do, so the matrix walks the real motion
  // rather than a hand-written approximation of it.
  M.pressToggle = (k) => { M.PREFS[k] = !M.PREFS[k]; M.savePref(k); M.applyPrefs(); };
  return M;
}

// ---------------------------------------------------------------------------
// 1. Every combination composes, and applying one never rewrites another.
// ---------------------------------------------------------------------------
{
  const M = makeModule();
  let combos = 0;
  for (let bits = 0; bits < (1 << ALL.length); bits++) {
    for (const st of STATIONS) {
      combos++;
      ALL.forEach((k, i) => { M.PREFS[k] = !!(bits & (1 << i)); });
      const before = ALL.map((k) => M.PREFS[k]);
      M.setStation(st);
      M.applyPrefs();
      const after = ALL.map((k) => M.PREFS[k]);
      ALL.forEach((k, i) => {
        if (before[i] !== after[i]) {
          fail("setStation/applyPrefs rewrote " + k +
               " (station " + st + ", bits " + bits + ")");
        }
      });
      // The phosphor must not depend on the content choice.  That IS the bug:
      // protocol mode losing the CRT.
      eq(M.nodes.screen.classList.contains("crt"), M.PREFS.crt,
         "crt class follows the crt pref (protocol=" + M.PREFS.protocol +
         ", station=" + st + ")");
      // ... and neither must the mirror.  With MIRROR on, the dock stays
      // visible in protocol mode; the CSS keys off this class.
      eq(M.nodes.html.classList.contains("mirror-on"), M.PREFS.dock,
         "mirror-on follows the dock pref (protocol=" + M.PREFS.protocol + ")");
      eq(M.nodes.dock.classList.contains("off"), !M.PREFS.dock,
         "the dock's own class follows the dock pref");
      // The station buttons paint exactly one selection, whatever the toggles.
      eq(M.nodes.stations.filter((b) => b.classList.contains("on")).length, 1,
         "exactly one station reads as selected (bits " + bits + ")");
      // Every toggle button reflects its own pref and nobody else's.
      ALL.forEach((k, i) => {
        eq(M.nodes.toggles[i].classList.contains("on"), M.PREFS[k],
           k + "'s button reflects " + k + " (bits " + bits + ")");
      });
    }
  }
  eq(combos, (1 << ALL.length) * STATIONS.length, "walked the whole matrix");
}

// ---------------------------------------------------------------------------
// 2. Pressing one toggle writes ONE storage key.  A toggle that persisted a
//    neighbour would survive test 1 (the in-memory state is fine) and still
//    lose the neighbour's setting on the next page load.
// ---------------------------------------------------------------------------
{
  const M = makeModule();
  for (const k of ALL) {
    const before = JSON.stringify(M.store);
    M.pressToggle(k);
    const touched = Object.keys(M.store).filter(
        (key) => JSON.parse(before)[key] !== M.store[key]);
    eq(touched.length, 1, "pressing " + k + " wrote one key (" + touched + ")");
    eq(touched[0], "swan.term." + k, "pressing " + k + " wrote its own key");
  }
  // Ordered so every step is a REAL change: setStation returns early when the
  // station is already selected, and a no-op writing nothing is not the thing
  // being measured here.
  for (const st of ["pearl", "flame", "swan"]) {
    const before = JSON.stringify(M.store);
    M.setStation(st);
    const touched = Object.keys(M.store).filter(
        (key) => JSON.parse(before)[key] !== M.store[key]);
    eq(touched.length, 1, "selecting " + st + " wrote one key (" + touched + ")");
    eq(touched[0], "swan.term.station", "selecting " + st + " wrote the station key");
  }
}

// ---------------------------------------------------------------------------
// 3. Each setting survives every transition, across a reload.
// ---------------------------------------------------------------------------
{
  const store = {};
  for (const k of ALL) {
    for (const v of [true, false]) {
      const M = makeModule(store);
      M.loadPrefs();
      M.PREFS[k] = v;
      M.savePref(k);
      // Now flip everything else, and visit all three stations.
      for (const other of ALL) { if (other !== k) M.pressToggle(other); }
      for (const st of STATIONS) M.setStation(st);
      for (const other of ALL) { if (other !== k) M.pressToggle(other); }

      // A new page load, same profile.
      const N = makeModule(store);
      N.loadPrefs();
      eq(N.PREFS[k], v,
         k + "=" + v + " survived every other toggle, three station changes " +
         "and a reload");
    }
  }
  for (const st of STATIONS) {
    const M = makeModule(store);
    M.loadPrefs();
    M.setStation(st === "swan" ? "pearl" : "swan");   // ensure a real change
    M.setStation(st);
    for (const k of ALL) M.pressToggle(k);
    const N = makeModule(store);
    N.loadPrefs();
    eq(N.station(), st, "station " + st + " survived every toggle and a reload");
  }
}

// ---------------------------------------------------------------------------
// 4. A CLEAN PROFILE.  Empty storage is what made the boot animation never
//    play: `boot` defaulted off, so playBoot() returned immediately on a fresh
//    browser and the incognito reproduction GUARANTEED the failure rather than
//    ruling anything out.  The default was never wrong - the gate was - so the
//    defaults are pinned here, including that `boot` is gone entirely.
// ---------------------------------------------------------------------------
{
  const M = makeModule();            // empty store
  M.loadPrefs();
  eq(M.station(), "swan", "a clean profile is the Swan station");
  for (const k of CONTENT) eq(M.PREFS[k], false, k + " defaults off");
  eq(M.PREFS.crt, false, "crt defaults off (it costs readability)");
  eq(M.PREFS.click, true, "key click defaults on");
  eq(M.PREFS.dock, true, "the mirror dock defaults on");
  eq("boot" in M.PREFS, false,
     "`boot` is not a pref any more - the animation is not gated on storage");
}

// ---------------------------------------------------------------------------
// 5. Private mode: storage throws.  Every toggle must still work for the
//    session, and nothing may escape as an exception - a thrown error here
//    takes the whole page down, which is how a blank UI happens.
// ---------------------------------------------------------------------------
{
  const M = makeModule();
  const boom = () => { throw new Error("SecurityError"); };
  global.localStorage = { getItem: boom, setItem: boom, removeItem: boom };
  let threw = null;
  try {
    M.loadPrefs();
    for (const k of ALL) M.pressToggle(k);
    for (const st of STATIONS) M.setStation(st);
  } catch (e) { threw = e; }
  eq(threw, null, "private mode does not throw out of the pref layer");
  eq(M.station(), "flame", "the station still changes for the session");
}

// ---------------------------------------------------------------------------
// 6. STATION IDENTITY.  The numbers are canon, verified against Lostpedia's
//    station list on 2026-08-25: Hydra 1, Arrow 2, Swan 3, Flame 4, Pearl 5,
//    Orchid 6.  They are pinned here rather than trusted, because the Flame and
//    the Pearl are ADJACENT in that list and 4/5 is exactly the pair a guess
//    would transpose - and a wrong number printed on a prop is worse than no
//    number, which is why they shipped without any until somebody checked.
//
//    Parsed by string indexing rather than by regex: a regex here needs
//    double-escaped backslashes inside a JS string literal, and one eaten
//    level turns the pattern into something that silently matches nothing -
//    a test that passes by failing to look.
// ---------------------------------------------------------------------------
{
  const proto = fs.readFileSync(
      path.join(__dirname, "..", "..", "web", "protocol.js"), "utf8");

  // The header string belonging to one station key inside the STATIONS table.
  function headerOf(station) {
    const at = proto.indexOf(station + ": {");
    if (at < 0) return null;
    const key = proto.indexOf("header:", at);
    const end = proto.indexOf("},", at);
    if (key < 0 || (end >= 0 && key > end)) return null;   // no header in THIS entry
    const q1 = proto.indexOf("\"", key);
    const q2 = proto.indexOf("\"", q1 + 1);
    return q1 < 0 || q2 < 0 ? null : proto.slice(q1 + 1, q2);
  }

  const CANON = { swan: "STATION 3 · THE SWAN",
                  flame: "STATION 4 · THE FLAME",
                  pearl: "STATION 5 · THE PEARL" };
  for (const st of STATIONS) {
    eq(headerOf(st), CANON[st], st + "'s header is the canon one");
  }
  // The extraction itself has to be able to fail, or the three checks above
  // would pass on any file at all.
  eq(headerOf("orchid"), null, "a station that is not in the table reads as absent");
}
if (failures) {
  console.log(failures + " failure(s)");
  process.exit(1);
}
console.log("all checks passed");
