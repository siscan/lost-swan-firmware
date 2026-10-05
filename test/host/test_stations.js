// The station screen's BEHAVIOUR, against the real handlers.
//
// test_toggles.js pins the preference layer; nothing pinned what a keystroke
// DOES.  That is where the bugs were, and every one of them is a property of
// event routing rather than of any single function:
//
//   - A click on a strip button left focus on the button, and every key handler
//     then stood aside for a focused control - so digits, Y/N, CHESS and even
//     ESC did nothing until you clicked empty screen.  The strip is the only
//     way to reach PROTOCOL, PEARL and FLAME without typing the name.
//   - Pearl and Flame open on a Y/N question, and a pending Y/N ate the N of
//     PANEL (the rule-2 keyboard escape) and of SWAN.
//   - One ESC closed the Pearl printout AND left protocol mode, and `c` and
//     Enter pressed in the open printout reached the friendly terminal's CANCEL
//     and EXECUTE - a stray `c` sent countdown.cancel.
//   - A page opened after the finale landed said SEALING for ever.
//   - The Enter that follows a typed LOGO skipped the animation it had just
//     asked for, and in the friendly terminal LOGO did nothing at all.
//
// The real bus.js / terminal.js / protocol.js / pearl.js / chat.js (and, where
// it matters, bootanim.js) run in a vm against the real terminal.html markup,
// with a virtual clock - see web_harness.js for what that is and is not.
"use strict";

const fs = require("fs");
const path = require("path");
const { boot, mkState, WEB } = require("./web_harness");

let failures = 0;
function check(ok, what, detail) {
  if (!ok) {
    failures++;
    console.log("FAIL " + what + (detail !== undefined ? "   (" + detail + ")" : ""));
  }
}

async function fresh(opts) {
  opts = opts || {};
  const env = boot(opts);
  await env.settle();
  env.openSocket();
  env.push(mkState(env, opts.state || {}));
  // chess.js is not loaded here; a stub that records opens keeps the Flame honest
  env.run("window.SwanChess = { opens: 0, _o: false, open() { this.opens++; this._o = true; return true; }," +
          " isOpen() { return this._o; }, close() { this._o = false; } };");
  env.protocolOn = (on) => env.run(
    "SwanTerm.prefs.protocol = " + (on !== false) + "; SwanTerm.savePref('protocol'); SwanTerm.applyPrefs();");
  env.station = (s) => env.run("SwanTerm.setStation('" + s + "')");
  env.out = () => {
    const o = env.doc.getElementById("pr-out");
    return o ? o.childNodes.map((c) => c.textContent) : null;
  };
  env.entryText = () => { const e = env.doc.getElementById("pr-entry"); return e ? e.textContent : null; };
  env.status = () => { const e = env.doc.getElementById("pr-status"); return e ? e.textContent : null; };
  env.flood = () => { const e = env.doc.getElementById("pr-flood"); return e ? e.textContent : null; };
  env.prefs = () => JSON.parse(env.run("JSON.stringify(window.SwanTerm.prefs)"));
  env.word = (w) => { env.type(w, 20); env.flush(300); };
  return env;
}
const journal = { "/api/journal": () => ({ ok: true, text: () => Promise.resolve('{"t":1,"u":2,"e":"boot","d":"poweron x"}\n') }) };

(async () => {
  // ---- the protocol screen: structure, teletype, echo, one command ----------
  {
    const env = await fresh();
    env.protocolOn(true);
    const prot = env.doc.getElementById("protocol");
    check(!!prot && prot.parentNode.id === "screen", "the station screen sits under #screen");
    check(prot.classList.contains("crt-layer"), "and carries crt-layer (rule 1's structural half)");
    check(env.doc.documentElement.classList.contains("protocol-on"), "html gets protocol-on");
    check(env.doc.getElementById("pr-head").textContent === "STATION 3 · THE SWAN", "Swan header is STATION 3");
    env.flush(22 * 5);
    check(env.out()[0] === "ENTER", "teletype: five characters after five 22 ms ticks", JSON.stringify(env.out()[0]));
    env.flush(1000);
    check(env.out()[0] === "ENTER THE NUMBERS, THEN EXECUTE.", "the hint finishes printing");

    env.run("SwanProtocol.say('x'.repeat(200))");
    env.flush(1000);
    const len = env.out()[1].length;
    check(len >= 44 && len <= 46, "teletype runs at 45 characters per second", len);
    env.key("a");
    check(env.out()[1].length === 200, "any key completes a print at once (rule 4)");
    env.key("Backspace");                  // the key that completed it was a letter, and letters echo

    env.station("swan");
    env.flush(100);
    env.type("4 8 15 16 23 42", 0);
    check(env.entryText() === "4 8 15 16 23 42", "accepted input echoes (rule 3)", env.entryText());
    const before = env.sent.length;
    env.key("Enter");
    check(env.sent.length === before + 1, "one Enter sends exactly one command (the double-execute defect)", env.sent.length - before);
    const sent = env.lastSent();
    check(sent.cmd === "countdown.execute" && sent.payload === "4 8 15 16 23 42", "and it is countdown.execute with the Numbers", JSON.stringify(sent));
    env.push({ e: "result", id: sent.id, res: { ok: true } });
    env.flush(500);
    check(env.out().some((l) => l === "ACCEPTED"), "ACCEPTED is printed");
    env.push(mkState(env, { mode: "countdown", phase: "running", target: Math.floor(env.clock.now / 1000) + 6480 }));
    env.flush(300);
    check(env.run("SwanProtocol.situation()") === "asleep", "a run above the 4:00 mark is the one inert state");
    env.key("7");
    check(env.entryText() === "", "asleep: typing is inert, with no echo");
    env.key("Escape");
    check(env.prefs().protocol === false && env.store["swan.term.protocol"] === "0", "asleep: ESC still leaves, and persists");
    check(env.errors.length === 0, "no handler threw", env.errors.map(String).join("; "));
  }

  // ---- the SYSTEM FAILURE flood: spec 7.3, a cross-repo contract ------------
  {
    const env = await fresh();
    env.protocolOn(true);
    env.flush(2000);
    env.push(mkState(env, { mode: "countdown", phase: "zero", target: Math.floor(env.clock.now / 1000) - 1 }));
    env.flush(250);
    check(env.run("SwanProtocol.situation()") === "failure", "zero is the failure situation");
    const f0 = env.flood().length;
    env.flush(1000);
    check(env.flood().length - f0 === 140, "the flood is exactly 140 characters a second", env.flood().length - f0);
    const fl = env.flood();
    check(fl === "SYSTEM FAILURE".repeat(fl.length / 14), "and only SYSTEM FAILURE, glued together");
    check(env.run("SwanProtocol.SYSTEM_FAILURE.length") === 14 && env.run("SwanProtocol.SYSTEM_FAILURE") === "SYSTEM FAILURE",
          "the literal is 14 characters with no trailing space");
    env.push(mkState(env, { mode: "clock", phase: "idle" }));
    env.flush(300);
    const f2 = env.flood().length;
    env.flush(1000);
    check(env.flood().length === f2, "the flood stops when the phase leaves failure");
    env.push(mkState(env, { mode: "countdown", phase: "zero", target: Math.floor(env.clock.now / 1000) - 1 }));
    env.flush(250);
    check(env.flood().length >= f2, "and a restart does not clear it");
    env.station("pearl");
    env.flush(300);
    check(env.run("SwanProtocol.situation()") === "idle", "the Pearl is indifferent to the countdown");
    env.station("flame");
    env.flush(300);
    check(env.run("SwanProtocol.situation()") === "idle", "and so is the Flame");
  }

  // ---- stations: whose command is whose, and how a station is chosen ---------
  {
    const env = await fresh({ fetchRoutes: journal });
    env.protocolOn(true);
    const plays = () => env.run("window.__bootPlays");
    const chess = () => env.run("window.SwanChess.opens");
    const pearlOpen = () => env.run("SwanPearl.isOpen()");

    env.station("swan"); env.flush(1200);
    const p0 = plays();
    env.word("LOGO");
    check(plays() === p0 + 1, "SWAN: LOGO replays the boot mark", plays() - p0);
    env.word("CHESS"); check(chess() === 0, "SWAN: CHESS does not open the board");
    env.word("LOG"); check(!pearlOpen(), "SWAN: LOG does not open the printout");
    env.key("Escape");
    env.protocolOn(true); env.flush(100);

    env.station("pearl"); env.flush(1200);
    env.word("CHESS"); check(chess() === 0, "PEARL: CHESS does nothing");
    const p1 = plays();
    env.word("LOGO"); check(plays() === p1, "PEARL: LOGO does not play the mark");
    for (let i = 0; i < 9; i++) env.key("Backspace");

    env.station("flame"); env.flush(1200);
    check(env.doc.getElementById("pr-head").textContent === "STATION 4 · THE FLAME", "Flame header is STATION 4");
    check(env.out().includes("CHESS? Y/N"), "the Flame opens on CHESS? Y/N");
    env.word("LOG"); check(!pearlOpen(), "FLAME: LOG does not open the printout");
    env.word("CHESS"); check(chess() === 1, "FLAME: CHESS opens the board", chess());
    env.run("window.SwanChess.close()");

    env.word("PEARL"); check(env.run("SwanTerm.station()") === "pearl", "typing PEARL switches station");
    env.word("SWAN"); check(env.run("SwanTerm.station()") === "swan", "typing SWAN switches station");
    env.word("FLAME"); check(env.run("SwanTerm.station()") === "flame", "typing FLAME switches station");
    check(env.store["swan.term.station"] === "flame", "the station is persisted");
    const prefsBefore = JSON.stringify(env.prefs());
    env.station("swan"); env.station("pearl");
    check(JSON.stringify(env.prefs()) === prefsBefore, "selecting a station changes no presentation toggle");
    env.word("PANEL");
    check(env.navigated.includes("index.html"), "PANEL goes to the control panel (rule 2's keyboard escape)", JSON.stringify(env.navigated));
    check(env.errors.length === 0, "no handler threw", env.errors.map(String).join("; "));
  }

  // ---- a FOCUSED BUTTON must not make the keyboard dead ---------------------
  {
    const env = await fresh();
    env.protocolOn(true); env.station("swan"); env.flush(1500);
    const btn = env.doc.querySelector('[data-toggle="crt"]');
    btn.focus();

    env.key("4", { target: btn });
    check(env.entryText() === "4", "protocol: a digit typed while a strip button has focus still echoes", env.entryText());
    const n0 = env.sent.length;
    const enter = env.key("Enter", { target: btn });
    check(env.sent.length === n0 && enter.defaultPrevented === false,
          "protocol: Enter on a focused button is the button's, not EXECUTE, and is not prevented");
    const space = env.key(" ", { target: btn });
    check(env.entryText() === "4" && space.defaultPrevented === false,
          "protocol: Space on a focused button is the button's, not typed, and is not prevented", env.entryText());
    env.key("Escape", { target: btn });
    check(env.prefs().protocol === false, "protocol: ESC with a button focused still leaves (rule 2)");

    // the friendly terminal's own handler
    env.protocolOn(false); env.flush(100);
    env.run("SwanTerm.station && 0");
    env.key("4", { target: btn }); env.key("8", { target: btn });
    const entry = env.doc.getElementById("entry");
    check(entry && entry.textContent === "48", "friendly: digits typed while a button has focus reach the Numbers", entry && entry.textContent);
    const n1 = env.sent.length;
    env.key("Enter", { target: btn });
    check(env.sent.length === n1, "friendly: Enter on a focused button is the button's, not EXECUTE");

    // and a mouse click must not park focus on the button in the first place
    btn.focus();
    env.dispatch(btn, { type: "click", detail: 1 });
    env.flush(5);
    check(env.doc.activeElement !== btn, "a pointer click leaves no focus on the button");
    btn.focus();
    env.dispatch(btn, { type: "click", detail: 0 });
    env.flush(5);
    check(env.doc.activeElement === btn, "a keyboard-activated click keeps focus (the focus ring is the user's)");
    check(env.errors.length === 0, "no handler threw", env.errors.map(String).join("; "));
  }

  // ---- a pending Y/N must not eat the letters of a command word -------------
  {
    const env = await fresh({ fetchRoutes: journal });
    env.protocolOn(true); env.station("pearl"); env.flush(1500);
    check(env.out().includes("PRINT LOG? Y/N"), "Pearl opens on PRINT LOG? Y/N");
    env.word("PANEL");
    check(env.navigated.includes("index.html"), "PANEL typed on the fresh question still navigates (the N is not 'no')", env.entryText());
  }
  {
    const env = await fresh({ fetchRoutes: journal });
    env.protocolOn(true); env.station("pearl"); env.flush(1500);
    env.word("SWAN");
    check(env.run("SwanTerm.station()") === "swan", "SWAN typed on the fresh question switches station");
  }
  {
    const env = await fresh({ fetchRoutes: journal });
    env.protocolOn(true); env.station("flame"); env.flush(1500);
    env.key("n");
    check(env.entryText() === "", "a bare N is still an answer: consumed, not echoed");
    env.flush(2000);
    check(env.out().some((l) => /TYPE CHESS/.test(l)), "and prints the hint", JSON.stringify(env.out()));
    check(env.run("window.SwanChess.opens") === 0, "and does not open the board");
  }
  {
    const env = await fresh();
    env.protocolOn(true); env.station("flame"); env.flush(1500);
    env.key("y");
    check(env.run("window.SwanChess.opens") === 1, "a bare Y still opens the board");
    check(env.entryText() === "", "and is consumed without an echo");
  }

  // ---- the Pearl printout owns the keyboard while it is up ------------------
  {
    const env = await fresh({ fetchRoutes: journal });
    env.protocolOn(true); env.station("pearl"); env.flush(1500);
    env.key("y");
    await env.settle();
    check(env.run("SwanPearl.isOpen()") === true, "Y opens the printout");
    env.key("Escape");
    check(env.run("SwanPearl.isOpen()") === false, "ESC closes the printout");
    check(env.prefs().protocol === true, "ESC that closed the printout does NOT also leave protocol mode");
    env.key("Escape");
    check(env.prefs().protocol === false, "a second ESC then leaves");
  }
  {
    // friendly mode: `c` is CANCEL and Enter is EXECUTE - neither may leak out of the printout
    const env = await fresh({ fetchRoutes: journal });
    env.station("pearl"); env.flush(200);
    env.type("log", 20); env.flush(300);
    await env.settle();
    check(env.run("SwanPearl.isOpen()") === true, "friendly: typing LOG opens the printout");
    const n0 = env.sent.length;
    env.key("c"); env.key("Enter"); env.key("c");
    check(env.sent.length === n0, "friendly: c and Enter in the open printout send nothing (no stray countdown.cancel)",
          env.sent.slice(n0).join(" | "));
    check(env.confirms.length === 0, "and raise no CANCEL confirm", env.confirms.length);
    env.key("Escape");
    check(env.run("SwanPearl.isOpen()") === false, "ESC closes it");
  }

  // ---- the failure line follows the STATE DOCUMENT, not only the event ------
  {
    const env = await fresh();
    env.protocolOn(true);
    env.flush(2000);
    env.push(mkState(env, { mode: "countdown", phase: "zero", target: Math.floor(env.clock.now / 1000) - 20, reveal_landed: true }));
    env.flush(500);
    check(env.run("SwanProtocol.situation()") === "failure", "past the reveal is still the failure situation");
    check(/SEALED/.test(env.status()) && !/SEALING/.test(env.status()),
          "a page that never saw the reveal EVENT still says SEALED (cd.reveal_landed)", env.status());
  }

  // ---- LOGO: typed, in both content modes, and the Enter that follows -------
  {
    const env = await fresh();
    env.protocolOn(true); env.station("swan"); env.flush(1200);
    env.word("LOGO");
    const o = JSON.parse(env.run("JSON.stringify(window.__bootOpts)"));
    check(o && o.skipable === true && o.graceMs >= 500,
          "a typed LOGO asks for a grace window, so the Enter after it cannot skip it", JSON.stringify(o));
  }
  {
    const env = await fresh({ realBoot: true });
    const ov = () => env.doc.getElementById("swan-boot");
    // With the real module the page's OWN on-load animation is running too (the
    // station is Swan at load).  Let it finish; typing LOGO into it would test
    // nothing, because play() returns the run already in flight.
    check(!!ov(), "the real boot animation plays on load");
    env.flush(6000);
    check(!ov(), "and finishes by itself");
    env.protocolOn(true); env.station("swan"); env.flush(1200);
    env.type("LOGO", 20);
    check(!!ov(), "protocol: typed LOGO starts the real animation");
    env.flush(120);
    env.key("Enter");
    check(!!ov() && !ov().classList.contains("still"), "protocol: the Enter after LOGO does not skip it");
    env.flush(700);
    env.key("x");
    check(!!ov() && ov().classList.contains("still"), "protocol: a key after the grace window skips (the finished mark first)");
    env.flush(1000);
    check(!ov(), "protocol: and it is gone, DOM clean");
  }
  {
    const env = await fresh({ realBoot: true });
    const ov = () => env.doc.getElementById("swan-boot");
    env.flush(6000);                       // the on-load animation first, as above
    check(!ov(), "friendly: the on-load animation has finished");
    env.type("logo", 20);
    check(!!ov(), "friendly: typing LOGO plays the animation (it used to do nothing)");
    env.flush(120);
    env.key("Enter");
    check(!!ov() && !ov().classList.contains("still"), "friendly: the Enter after LOGO does not skip it");
    env.flush(6000);
    check(!ov(), "friendly: it finishes on its own");
  }
  for (const st of ["pearl", "flame"]) {
    const env = await fresh({ realBoot: true, fetchRoutes: journal });
    env.flush(6000);                       // the on-load animation first
    env.station(st); env.flush(200);
    env.type("logo", 20);
    check(!env.doc.getElementById("swan-boot"), "friendly on " + st + ": LOGO is not that station's command");
  }

  // ---- MIRROR in protocol mode (qa.js P-6) - the part a vm cannot render ----
  {
    // Layout is a browser's business; what can be pinned here is the arrangement
    // the fix depends on.  #dock is a child of .content, which is a z-index:1
    // stacking context, so the dock's own z-index:45 can never outrank #protocol
    // (z-index:1, later in the DOM).  The fix lifts .content itself in protocol
    // mode and hides everything in it but the dock.
    const env = await fresh();
    const css = fs.readFileSync(path.join(WEB, "terminal.css"), "utf8");
    check(!!env.doc.getElementById("dock").closest(".content"), "#dock is still inside .content (the rule below assumes it)");
    check(/\.protocol-on\s+\.content\s*\{[^}]*z-index:\s*2/.test(css) && /\.protocol-on\s+\.content\s*\{[^}]*pointer-events:\s*none/.test(css),
          "terminal.css lifts .content above the station screen in protocol mode, inert");
    check(/\.protocol-on\s+\.content\s*>\s*:not\(#dock\)\s*\{[^}]*visibility:\s*hidden/.test(css),
          "and hides everything in it except the dock");
  }

  if (failures) {
    console.log(failures + " failure(s)");
    process.exit(1);
  }
  console.log("all checks passed");
})();
