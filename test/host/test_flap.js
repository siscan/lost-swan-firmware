// The mirror widget (web/flap.js), against a fake DOM and a virtual clock.
//
// WHY THIS EXISTS.  On 2026-08-23 the mirror on the board went stale and stayed
// stale: cards showed a face the display had left minutes earlier while the
// Diagnostics table beside them, built from the same payload, was right.  The
// cause was that `go` events - the only thing that moved a card after the first
// prime - travel a lossy path.  Measured on the wire: a five-column frame
// emitted five `go` events and exactly TWO reached the browser, every time.
// Nothing ever corrected the other three, because the frame scheduler does not
// re-command a column that is already where it should be.
//
// Both halves were fixed - the transport no longer drops (components/net/httpd.cpp)
// and the widget now reconciles against the state document, which is
// authoritative and repeats.  This suite pins the SECOND half, because it is
// the one that has to keep working when the first fails again: it drives the
// widget with events deliberately missing and asserts every card still lands.
//
// No npm, no jsdom.  flap.js touches about a dozen DOM calls; they are faked
// below in ~120 lines.  Timers are virtual, so a 49-flip wrap costs no wall
// clock and the result is deterministic.
//
//   node test/host/test_flap.js [path/to/ring.json]
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const ROOT = path.resolve(__dirname, "..", "..");
const RING_PATH = process.argv[2] || path.join(ROOT, "data", "ring.json");

// --------------------------------------------------------------------------
// Check harness - same shape as test/host/check.h so failures read alike
// --------------------------------------------------------------------------
let failures = 0;
function CHECK(cond, what) {
  if (!cond) {
    console.log("FAIL " + what);
    failures++;
  }
}
function CHECK_EQ(a, b, what) {
  const sa = JSON.stringify(a);
  const sb = JSON.stringify(b);
  if (sa !== sb) {
    console.log("FAIL " + what + "  (" + sa + " vs " + sb + ")");
    failures++;
  }
}

// --------------------------------------------------------------------------
// Virtual clock.  setInterval/setTimeout register here; advance() runs due
// callbacks in time order, including ones scheduled by other callbacks (flap.js
// nests a setTimeout inside every setInterval tick).
// --------------------------------------------------------------------------
function makeClock() {
  let now = 0;
  let seq = 0;
  const timers = new Map();

  const add = (fn, delay, repeat) => {
    const id = ++seq;
    timers.set(id, { fn, at: now + Math.max(0, delay || 0), every: repeat ? Math.max(1, delay) : 0 });
    return id;
  };

  return {
    now: () => now,
    setTimeout: (fn, d) => add(fn, d, false),
    setInterval: (fn, d) => add(fn, d, true),
    clearInterval: (id) => timers.delete(id),
    clearTimeout: (id) => timers.delete(id),
    pending: () => timers.size,
    advance(ms) {
      const end = now + ms;
      // Bounded so a runaway animation fails the test instead of hanging it.
      for (let guard = 0; guard < 2000000; guard++) {
        let next = null;
        for (const [id, t] of timers) {
          if (t.at <= end && (next === null || t.at < next.t.at)) next = { id, t };
        }
        if (next === null) break;
        now = next.t.at;
        if (next.t.every) next.t.at = now + next.t.every;
        else timers.delete(next.id);
        next.t.fn();
      }
      now = end;
    },
  };
}

// --------------------------------------------------------------------------
// Fake DOM - only what FlapDisplay actually touches
// --------------------------------------------------------------------------
function makeEl(tag) {
  const classes = new Set();
  let text = "";
  const el = {
    tagName: tag,
    children: [],
    style: {},
    attrs: {},
    title: "",
    id: "",
    appendChild(c) {
      el.children.push(c);
      return c;
    },
    setAttribute(k, v) {
      el.attrs[k] = String(v);
    },
    setAttributeNS(_ns, k, v) {
      el.attrs[k] = String(v);
    },
    querySelectorAll() {
      return { forEach() {} };
    },
    get classList() {
      return {
        add: (c) => classes.add(c),
        remove: (c) => classes.delete(c),
        contains: (c) => classes.has(c),
        toggle: (c, on) => {
          const want = on === undefined ? !classes.has(c) : !!on;
          if (want) classes.add(c);
          else classes.delete(c);
        },
      };
    },
    get className() {
      return Array.from(classes).join(" ");
    },
    set className(v) {
      classes.clear();
      String(v)
        .split(/\s+/)
        .filter(Boolean)
        .forEach((c) => classes.add(c));
    },
    get textContent() {
      return text;
    },
    set textContent(v) {
      // The real thing clears children when textContent is assigned; build()
      // relies on that to empty the host before rebuilding.
      text = String(v);
      el.children.length = 0;
    },
  };
  return el;
}

function loadFlap(clock) {
  const body = makeEl("body");
  const document = {
    body,
    createElement: (t) => makeEl(t),
    createElementNS: (_ns, t) => makeEl(t),
    getElementById: () => null,
  };
  const ctx = {
    document,
    console,
    setTimeout: clock.setTimeout,
    setInterval: clock.setInterval,
    clearTimeout: clock.clearTimeout,
    clearInterval: clock.clearInterval,
  };
  // flap.js is `(function (global) { ... })(window)`, so window must be the
  // context's own global for `global.SwanFlap = ...` to land where we can see it.
  ctx.window = ctx;
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync(path.join(ROOT, "web", "flap.js"), "utf8"), ctx, {
    filename: "web/flap.js",
  });
  if (!ctx.SwanFlap) throw new Error("flap.js did not export SwanFlap");
  return { SwanFlap: ctx.SwanFlap, document };
}

// --------------------------------------------------------------------------
// A rig: the real ring document, the real widget, a fake state document
// --------------------------------------------------------------------------
const ring = JSON.parse(fs.readFileSync(RING_PATH, "utf8"));
const N = 5;

function rig() {
  const clock = makeClock();
  const { SwanFlap, document } = loadFlap(clock);
  const host = document.createElement("div");
  const flap = new SwanFlap.FlapDisplay(host, ring, { gapAfter: 2 });
  return { clock, flap, host, SwanFlap };
}

// The /ws cols array, as build_state emits it.
function cols(index, dest, state) {
  return index.map((ix, i) => ({
    index: ix,
    dest: dest ? dest[i] : ix,
    state: state ? state[i] : ix < 0 ? "HOMING" : "IDLE",
    settled: state ? state[i] === "IDLE" && ix >= 0 : ix >= 0,
    retry: 0,
    mode: "sim",
    cause: "none",
    face: "",
  }));
}

const faces = (flap) => flap.cols.map((c) => c.idx);

// A frame the display can actually reach: every column ends on `to[i]`.
function driveFrame(r, from, to, opts) {
  const o = opts || {};
  // 1. The command lands: the axes report the OLD index and the NEW dest.
  //    Only the columns in o.deliver get a `go` event - the rest model exactly
  //    what the board did when the transport dropped them.
  if (o.deliver) {
    o.deliver.forEach((i) => r.flap.flipTo(i, to[i], 15));
  }
  r.flap.reconcile(cols(from, to), 15);
  r.clock.advance(1000);
  // 2. The drums arrive.  The state document now reports the new index.
  r.flap.reconcile(cols(to, to), 15);
  r.clock.advance(20000);
}

// --------------------------------------------------------------------------
// Tests
// --------------------------------------------------------------------------

// THE reported bug, reproduced exactly: the board delivered `go` for columns 0
// and 1 and dropped the other three.  Every card must still land.
function test_partial_burst_still_lands() {
  const r = rig();
  r.flap.setAll([0, 0, 0, 0, 0]);
  const qmarks = [17, 17, 17, 17, 25];
  driveFrame(r, [0, 0, 0, 0, 0], qmarks, { deliver: [0, 1] });
  CHECK_EQ(faces(r.flap), qmarks, "a burst with three go events lost still lands");
}

// ... and with EVERY event lost, which is the case reconciliation exists for.
function test_no_events_at_all() {
  const r = rig();
  r.flap.setAll([0, 0, 0, 0, 0]);
  const target = [45, 12, 3, 48, 44];
  driveFrame(r, [0, 0, 0, 0, 0], target, { deliver: [] });
  CHECK_EQ(faces(r.flap), target, "a frame with no go events at all still lands");
}

// N mode changes in a row - the owner's stated requirement.  Alternating
// presets and clock faces, with a different subset of events lost each time,
// because the real loss was positional and the count varied.
function test_n_changes_in_a_row() {
  const r = rig();
  r.flap.setAll([0, 0, 0, 0, 0]);
  const frames = [
    [17, 17, 17, 17, 25], // qmarks
    [0, 0, 0, 0, 0],      // blank
    [39, 0, 40, 48, 44],  // clock 09:15
    [17, 17, 17, 17, 25], // qmarks
    [39, 0, 40, 46, 40],  // clock 09:39 -> arbitrary faces
    [0, 0, 0, 0, 0],      // blank
    [1, 2, 3, 4, 5],      // raw frame
    [49, 49, 49, 49, 49], // the far end of the ring
  ];
  const drops = [[0, 1], [], [0], [0, 1, 2, 3, 4], [2, 4], [1], [], [3]];
  let from = [0, 0, 0, 0, 0];
  frames.forEach((to, k) => {
    driveFrame(r, from, to, { deliver: drops[k] });
    CHECK_EQ(faces(r.flap), to, "change " + (k + 1) + " of " + frames.length + " repaints all five");
    from = to;
  });
}

// A ring reload must not leave the widget unable to track.  setRing() keeps the
// cards (the column count is unchanged) and repaints them against the new
// table; reconciliation has to keep working across it.
function test_after_ring_reload() {
  const r = rig();
  r.flap.setAll([0, 0, 0, 0, 0]);
  driveFrame(r, [0, 0, 0, 0, 0], [17, 17, 17, 17, 25], { deliver: [] });

  r.flap.setRing(JSON.parse(JSON.stringify(ring)));
  CHECK_EQ(faces(r.flap), [17, 17, 17, 17, 25], "setRing keeps the painted indices");

  const after = [39, 0, 40, 48, 44];
  driveFrame(r, [17, 17, 17, 17, 25], after, { deliver: [1] });
  CHECK_EQ(faces(r.flap), after, "reconciliation still repaints all five after a ring reload");

  // And again, several times, because the reported failure only showed up on
  // the SECOND change after a rebuild.
  let from = after;
  [[0, 0, 0, 0, 0], [17, 17, 17, 17, 25], [5, 6, 7, 8, 9]].forEach((to, k) => {
    driveFrame(r, from, to, { deliver: [] });
    CHECK_EQ(faces(r.flap), to, "post-reload change " + (k + 1) + " repaints all five");
    from = to;
  });
}

// Unknown is not blank.  A column hunting for its hall edge reports index -1
// while dest is the home slot; painting dest would show a confident blank for a
// column that has no idea where it is.
function test_unknown_beats_dest() {
  const r = rig();
  r.flap.setAll([10, 10, 10, 10, 10]);
  r.flap.reconcile(cols([-1, -1, 10, 10, 10], [0, 0, 10, 10, 10],
                        ["HOMING", "UNHOMED", "IDLE", "IDLE", "IDLE"]), 15);
  r.clock.advance(5000);
  CHECK_EQ(faces(r.flap), [-1, -1, 10, 10, 10], "index -1 paints unknown, never dest");
  CHECK(r.flap.cols[0].card.classList.contains("unknown"), "the hunting card is marked unknown");
  CHECK(!r.flap.cols[2].card.classList.contains("unknown"), "a settled card is not marked unknown");

  // ... and it recovers once the column homes.
  r.flap.reconcile(cols([0, 0, 10, 10, 10], [0, 0, 10, 10, 10]), 15);
  r.clock.advance(5000);
  CHECK_EQ(faces(r.flap), [0, 0, 10, 10, 10], "a homed column leaves the unknown state");
  CHECK(!r.flap.cols[0].card.classList.contains("unknown"), "unknown is cleared on repaint");
}

// An open-loop spin is a deliberate "nobody knows where this is going".
// Reconciliation must not seize the card mid-spin.
function test_spin_is_not_interrupted() {
  const r = rig();
  r.flap.setAll([0, 0, 0, 0, 0]);
  r.flap.spin(0, 25, 4);
  const t = r.flap.cols[0].timer;
  CHECK(t !== null, "the spin is running");
  r.clock.advance(500);
  r.flap.reconcile(cols([-1, 0, 0, 0, 0], [12, 0, 0, 0, 0]), 15);
  CHECK_EQ(r.flap.cols[0].timer, t, "reconcile leaves a running spin alone");
  CHECK_EQ(r.flap.cols[0].target, -1, "the spin keeps its open-loop target");
  // When it ends the card is unknown, and reconciliation may then take over.
  r.clock.advance(10000);
  CHECK_EQ(r.flap.cols[0].idx, -1, "a finished spin leaves the position unknown");
  r.flap.reconcile(cols([7, 0, 0, 0, 0], [7, 0, 0, 0, 0]), 15);
  r.clock.advance(20000);
  CHECK_EQ(r.flap.cols[0].idx, 7, "reconcile recovers the card after the spin");
}

// Reconciliation must be idle when there is nothing to do: no timers started,
// no churn at 5 Hz.
function test_settled_is_left_alone() {
  const r = rig();
  r.flap.setAll([3, 4, 5, 6, 7]);
  const before = faces(r.flap);
  for (let k = 0; k < 20; k++) {
    r.flap.reconcile(cols([3, 4, 5, 6, 7], [3, 4, 5, 6, 7]), 15);
  }
  CHECK_EQ(r.clock.pending(), 0, "a settled display starts no timers");
  CHECK_EQ(faces(r.flap), before, "a settled display is not repainted away");
}

// A card already animating toward the right target must not be restarted - the
// duplicate-go guard is deliberate (the scheduler issues a target twice in one
// tick) and reconciliation runs at 5 Hz on top of it.
function test_no_restart_of_a_correct_animation() {
  const r = rig();
  r.flap.setAll([0, 0, 0, 0, 0]);
  r.flap.flipTo(0, 30, 15);
  const t = r.flap.cols[0].timer;
  r.clock.advance(200);
  const mid = r.flap.cols[0].idx;
  CHECK(mid > 0 && mid < 30, "the animation is under way");
  for (let k = 0; k < 5; k++) {
    r.flap.reconcile(cols([0, 0, 0, 0, 0], [30, 0, 0, 0, 0]), 15);
  }
  CHECK_EQ(r.flap.cols[0].timer, t, "reconcile does not restart a correct animation");
  r.clock.advance(20000);
  CHECK_EQ(r.flap.cols[0].idx, 30, "and it still arrives");
}

// The ring is one-way: a card must never be walked backwards, whatever the
// reconciliation asks for.  Every intermediate step is +1 mod N.
function test_forward_only() {
  const r = rig();
  r.flap.setAll([40, 0, 0, 0, 0]);
  const seen = [];
  const inner = r.flap.paint.bind(r.flap);
  r.flap.paint = (i, idx) => {
    if (i === 0) seen.push(idx);
    inner(i, idx);
  };
  r.flap.reconcile(cols([40, 0, 0, 0, 0], [5, 0, 0, 0, 0]), 15);
  r.clock.advance(20000);
  CHECK_EQ(r.flap.cols[0].idx, 5, "the wrapping move lands");
  const n = ring.slot_count || 50;
  let ok = true;
  let prev = 40;
  seen.forEach((idx) => {
    if (idx !== (prev + 1) % n) ok = false;
    prev = idx;
  });
  CHECK(ok, "every step is one flap forward: " + seen.join(","));
}

// --------------------------------------------------------------------------
const TESTS = [
  test_partial_burst_still_lands,
  test_no_events_at_all,
  test_n_changes_in_a_row,
  test_after_ring_reload,
  test_unknown_beats_dest,
  test_spin_is_not_interrupted,
  test_settled_is_left_alone,
  test_no_restart_of_a_correct_animation,
  test_forward_only,
];

CHECK(ring.slot_count === 50, "the ring document has 50 slots");
CHECK((ring.columns || []).length === N, "the ring document has five columns");
TESTS.forEach((t) => t());

if (failures !== 0) {
  console.log(failures + " failure(s)");
  process.exit(1);
}
console.log("all checks passed");
