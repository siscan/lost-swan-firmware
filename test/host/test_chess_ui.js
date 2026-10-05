// The Flame's chess UI, against the real chess.js in a fake browser.
//
// The engine's rules are test_chess_engine.js.  THIS is what the player sees and
// presses: gating (the Flame's, nobody else's), board orientation, a normal turn,
// promotion, checkmate -> the command menu -> code 77 -> the boot animation, and
// the RACES that used to leave a finished game's timers running into the next one
// (NEW GAME during the engine's think, NEW GAME or TAKE BACK inside the 1.4 s
// between mate and menu, two menu codes interleaving, a stale promotion button).
"use strict";
const { makeWorld, seamTransform } = require("./chess_world");

let nOk = 0, nFail = 0;
function check(label, cond, detail) {
  if (cond) { nOk++; console.log("  ok   " + label); }
  else { nFail++; console.log("  FAIL " + label + (detail ? "  -> " + detail : "")); }
}
function info(s) { console.log("  info " + s); }
function section(s) { console.log("\n== " + s); }

function fresh(opts) {
  opts = opts || {};
  const W = makeWorld(opts);
  if (opts.protocol !== false) W.loadFile("protocol.js");
  W.loadFile("chess.js", seamTransform);
  const eng = W.sandbox.__eng, chess = W.sandbox.SwanChess;
  return { W, eng, chess };
}
const GAME = (eng, fen) => ({ st: eng.fromFen(fen), sel: -1, dests: [], last: null, over: false, hist: [], log: [], promo: null, thinking: false });
function setPos(t, fen) { t.eng.game = GAME(t.eng, fen); t.eng.render(); }
const sq = (t, i) => t.eng.el.squares[i];
const clickSq = (t, i) => t.W.click(sq(t, i));
const btn = (t, text) => {
  const all = []; (function walk(n) { for (const c of n.childNodes) { if (c.nodeType === 1) { all.push(c); walk(c); } } })(t.eng.el.box);
  return all.find((n) => n.tagName === "BUTTON" && n.textContent === text);
};
const mopts = (t) => t.eng.el.menu.childNodes.filter((n) => n.nodeType === 1 && n.className === "sc-mopt");

// ---------------------------------------------------------------------------
section("U1 gating: chess is the Flame's and nobody else's");
{
  const t = fresh({ station: "swan" });
  check("open() refused on SWAN", t.chess.open() === false && !t.W.body.querySelector("#swan-chess"));
  t.W.term._station = "pearl";
  check("open() refused on PEARL", t.chess.open() === false);
  t.W.term._station = "flame";
  check("open() works on FLAME", t.chess.open() === true && t.chess.isOpen());
  check("second open() is a no-op (false)", t.chess.open() === false);
  check("one overlay, one style tag", t.W.body.childNodes.filter((n) => n.id === "swan-chess").length === 1 &&
        t.W.html.childNodes[0].childNodes.filter((n) => n.id === "swan-chess-css").length === 1);
}

section("U2 board orientation / glyphs / coordinates");
{
  const t = fresh(); t.chess.open();
  const L = [0, 3, 4, 7, 56, 59, 60, 63].map((i) => i + ":" + sq(t, i).textContent).join(" ");
  check("a8=r d8=q e8=k h8=r a1=R d1=Q e1=K h1=R (white at the bottom)", L === "0:r 3:q 4:k 7:r 56:R 59:Q 60:K 63:R", L);
  check("a8 light, h8 dark, a1 dark, h1 light",
        sq(t, 0).classList.contains("lt") && !sq(t, 7).classList.contains("lt") && !sq(t, 56).classList.contains("lt") && sq(t, 63).classList.contains("lt"));
  check("files a..h labelled on the bottom row only", [56, 57, 58, 59, 60, 61, 62, 63].map((i) => sq(t, i).dataset.f).join("") === "abcdefgh" && sq(t, 0).dataset.f === undefined);
  check("ranks 8..1 labelled on the left column only", [0, 8, 16, 24, 32, 40, 48, 56].map((i) => sq(t, i).dataset.r).join("") === "87654321" && sq(t, 1).dataset.r === undefined);
  info("black pieces render as LOWERCASE FEN letters, white as uppercase (e.g. d8='" + sq(t, 3).textContent + "', d1='" + sq(t, 59).textContent + "')");
  check("aria-labels name the piece and square", sq(t, 52).getAttribute("aria-label") === "e2 white pawn", sq(t, 52).getAttribute("aria-label"));
}

section("U3 a normal turn: select, dots, move, engine replies after 280 ms");
{
  const t = fresh(); t.chess.open();
  clickSq(t, 52);
  check("e2 selected with 2 destinations", t.eng.game.sel === 52 && t.eng.game.dests.length === 2);
  const dots = (i) => sq(t, i).childNodes.some((n) => n.nodeType === 1 && /sc-dot/.test(n.className));
  check("move dots on e3 and e4", dots(44) && dots(36));
  clickSq(t, 36);
  check("pawn on e4, status says THINKING", sq(t, 36).textContent === "P" && /THINKING/.test(t.eng.el.status.textContent), t.eng.el.status.textContent);
  clickSq(t, 51);
  check("clicks ignored while the engine is thinking", t.eng.game.sel === -1);
  t.W.advance(270);
  check("engine has NOT moved at 270 ms", t.eng.game.st.turn === "b");
  t.W.advance(20);
  check("engine moved by 290 ms; white to move", t.eng.game.st.turn === "w" && /YOUR MOVE/.test(t.eng.el.status.textContent), t.eng.el.status.textContent);
  info("move log after one full move: " + JSON.stringify(t.eng.el.log.textContent));
  check("no dispatcher command was sent", t.W.sent.length === 0);
  check("no exceptions", t.W.errors.length === 0, t.W.errors[0]);
}

section("U4 promotion UI");
{
  const t = fresh(); t.chess.open();
  setPos(t, "7k/P7/8/8/8/8/8/K7 w - - 0 1");
  clickSq(t, 8); clickSq(t, 0);
  check("promotion prompt appears with 4 choices", t.eng.game.promo && t.eng.game.promo.length === 4 && t.eng.el.promo.classList.contains("on"));
  check("choices are Q R B N", ["Q", "R", "B", "N"].every((s) => !!btn(t, s)));
  check("status asks for a piece", /CHOOSE A PIECE/.test(t.eng.el.status.textContent));
  clickSq(t, 9);
  check("clicking the board while the prompt is up does nothing", !!t.eng.game.promo);
  t.W.click(btn(t, "B"));
  check("choosing B promotes to a white bishop on a8", t.eng.game.st.b[0] === "B", t.eng.game.st.b[0]);
  info("log: " + JSON.stringify(t.eng.el.log.textContent));
  check("no exceptions", t.W.errors.length === 0, t.W.errors[0]);

  // NEW GAME while the promotion prompt is up
  const u = fresh(); u.chess.open();
  setPos(u, "7k/P7/8/8/8/8/8/K7 w - - 0 1");
  clickSq(u, 8); clickSq(u, 0);
  const stale = btn(u, "Q");
  u.W.click(btn(u, "NEW GAME"));
  check("NEW GAME clears the promotion prompt", !u.eng.el.promo.classList.contains("on"), "prompt still showing: '" + u.eng.el.promo.textContent + "'");
  u.W.click(stale);
  check("a stale promotion button does not throw", u.W.errors.length === 0, (u.W.errors[0] || "").split("\n")[0]);
}

section("U5 checkmate opens the menu; code 77 runs to the boot animation");
{
  const t = fresh(); t.chess.open();
  setPos(t, "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1");
  clickSq(t, 56); clickSq(t, 0);
  check("status: CHECKMATE - THE FLAME CONCEDES", /CONCEDES/.test(t.eng.el.status.textContent), t.eng.el.status.textContent);
  t.W.advance(1390);
  check("menu NOT open at 1390 ms", !t.eng.menuOn);
  t.W.advance(20);
  check("menu open at 1410 ms", t.eng.menuOn && t.eng.el.menu.classList.contains("on"));
  check("menu lists 24 / 32 / 38 / 77", mopts(t).map((n) => n.dataset.code).join() === "24,32,38,77");
  t.W.key("7"); t.W.advance(40);
  check("digit echoes at >:", /^>: 7_?$/.test(t.eng.el.entry.textContent), t.eng.el.entry.textContent);
  t.W.key("Backspace"); t.W.advance(10);
  check("Backspace deletes it", t.eng.el.entry.textContent === ">: _", t.eng.el.entry.textContent);
  t.W.key("7"); t.W.advance(30); t.W.key("7");
  check("two digits shown", t.eng.el.entry.textContent === ">: 77", t.eng.el.entry.textContent);
  const t0 = t.W.now();
  t.W.advance(230);
  check("77 accepted ~220 ms after the 2nd digit", /CODE 77 ACCEPTED/.test(t.eng.el.out.textContent), JSON.stringify(t.eng.el.out.textContent));
  let handoffAt = -1;
  for (let i = 0; i < 400 && handoffAt < 0; i++) { t.W.advance(20); if (t.W.boot.length) handoffAt = t.W.now() - t0; }
  check("hands off to SwanBoot.play({skipable:true})", t.W.boot.length === 1 && t.W.boot[0].skipable === true, JSON.stringify(t.W.boot));
  info("77 -> boot animation handoff took " + handoffAt + " ms of virtual time");
  check("chess closed at handoff", !t.chess.isOpen());
  check("no timers leaked after handoff", t.W.timerCount() <= 1, "pending timers: " + t.W.timerCount() + " (protocol render loop = 1)");
  check("no exceptions", t.W.errors.length === 0, t.W.errors[0]);
}

section("U6 race: NEW GAME / TAKE BACK inside the 1.4 s between mate and menu");
for (const which of ["NEW GAME", "TAKE BACK"]) {
  const t = fresh(); t.chess.open();
  setPos(t, "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1");
  clickSq(t, 56); clickSq(t, 0);
  t.W.advance(300);
  t.W.click(btn(t, which));
  t.W.advance(1500);
  check(which + " within 1.4 s does not pop the menu over the fresh position", !t.eng.menuOn,
        "menu opened over a live game (menuOn=" + t.eng.menuOn + ")");
}

section("U7 race: NEW GAME while the engine is thinking (280 ms window)");
{
  const t = fresh(); t.chess.open();
  clickSq(t, 52); clickSq(t, 36);
  t.W.advance(100);
  t.W.click(btn(t, "NEW GAME"));
  t.W.advance(400);
  const turn = t.eng.game.st.turn;
  check("fresh game still has WHITE to move after the stale engine timer fires", turn === "w",
        "turn=" + turn + " status=" + JSON.stringify(t.eng.el.status.textContent) + " log=" + JSON.stringify(t.eng.el.log.textContent));
  if (turn !== "w") {
    clickSq(t, 52); t.W.advance(2000);
    check("   ...and the player can still move (not soft-locked)", t.eng.game.sel === 52, "click on e2 ignored; game.sel=" + t.eng.game.sel);
  }
}

section("U8/U9 menu output races");
{
  const t = fresh(); t.chess.open(); t.eng.openMenu();
  t.W.click(mopts(t)[0]); t.W.advance(100); t.W.click(mopts(t)[1]); t.W.advance(3000);
  const out = t.eng.el.out.textContent;
  const clean24 = "REQUEST QUEUED WITH DHARMA LOGISTICS.", clean32 = "STATION 3 - THE SWAN";
  check("second code's output is not interleaved with the first's", !(out.indexOf("REQUEST QUEUED") >= 0 && out.indexOf(clean32) >= 0) && out.indexOf("MANIFEST") < 0 && out.indexOf("AUTOMATED") < 0,
        "out=" + JSON.stringify(out));
}
{
  const t = fresh(); t.chess.open(); t.eng.openMenu();
  t.W.key("7"); t.W.advance(30); t.W.key("7"); t.W.advance(30); t.W.key("Enter"); t.W.advance(1500);
  const out = t.eng.el.out.textContent;
  check("Enter inside the 220 ms auto-run window does not clobber the output", !/NOT RECOGNISED/.test(out), "out=" + JSON.stringify(out));
}
{
  const t = fresh(); t.chess.open(); t.eng.openMenu();
  t.W.key("7"); t.W.advance(30); t.W.key("7"); t.W.advance(100); t.W.key("Backspace"); t.W.advance(1500);
  const out = t.eng.el.out.textContent;
  info("typed 77 then Backspace within 220 ms -> out=" + JSON.stringify(out) + " (77 was " + (/ACCEPTED/.test(out) ? "run" : "not run") + ")");
}
{
  const t = fresh(); t.chess.open(); t.eng.openMenu();
  t.W.key("9"); t.W.advance(30); t.W.key("9"); t.W.advance(500);
  check("unknown code is reported", /COMMAND 99 NOT RECOGNISED/.test(t.eng.el.out.textContent), JSON.stringify(t.eng.el.out.textContent));
  check("menu click works without any keyboard", (() => { t.W.click(mopts(t)[3]); t.W.advance(300); return /CODE 77 ACCEPTED/.test(t.eng.el.out.textContent); })());
}

section("U10 key capture while the board is open");
{
  const t = fresh({ protocol: true }); t.W.term.prefs.protocol = true; t.W.term.applyPrefs(); t.W.advance(800);
  t.chess.open();
  const seen = [];
  for (const k of ["a", "y", "c", "C", "7", "Backspace", "Enter", " ", "Escape"]) {
    if (k === "Escape") continue;
    const ev = t.W.key(k); seen.push(k + ":" + (ev._sip ? "S" : "-") + (ev.defaultPrevented ? "P" : "-"));
  }
  info("(S=stopImmediatePropagation, P=preventDefault) " + seen.join(" "));
  check("terminal.js's handler saw NOTHING (no CANCEL from 'c', no EXECUTE from Enter)", t.W.termSaw.length === 0, JSON.stringify(t.W.termSaw));
  check("protocol screen entry untouched", t.W.sandbox.SwanProtocol._entry() === "");
  const tab = t.W.key("Tab");
  check("Tab passes through (focus can move inside the board)", !tab._sip);
  const ctl = t.W.key("r", { ctrl: true });
  check("Ctrl+R passes through (browser reload keeps working)", !ctl._sip);
  const en = t.W.key("Enter");
  check("Enter/Space are NOT preventDefault'd on the board (a focused square can be activated)", !en.defaultPrevented);
  const esc = t.W.key("Escape");
  check("Escape closes chess only", !t.chess.isOpen() && t.W.term.prefs.protocol === true, "protocol=" + t.W.term.prefs.protocol);
  const esc2 = t.W.key("Escape");
  check("a second Escape leaves protocol mode", t.W.term.prefs.protocol === false);
}

section("U11 protocol screen: CHESS? Y/N");
{
  const t = fresh(); t.W.term.prefs.protocol = true; t.W.term.applyPrefs(); t.W.advance(800);
  const out = () => t.W.body.querySelector("#pr-out").textContent;
  check("Flame boots to 'CHESS? Y/N'", out() === "CHESS? Y/N", JSON.stringify(out()));
  t.W.key("n"); t.W.advance(800);
  check("N prints the hint and does not echo the letter", /TYPE CHESS TO PLAY/.test(out()) && t.W.sandbox.SwanProtocol._entry() === "", JSON.stringify(out()));
  t.W.type("chess", 30);
  check("typing CHESS at the prompt opens the board", t.chess.isOpen());
  check("no key reached the friendly terminal (no CANCEL)", t.W.termSaw.length === 0, JSON.stringify(t.W.termSaw));
  t.W.key("Escape");
  const u = fresh(); u.W.term.prefs.protocol = true; u.W.term.applyPrefs(); u.W.advance(800);
  u.W.key("y");
  check("Y opens the board, no echo", u.chess.isOpen() && u.W.sandbox.SwanProtocol._entry() === "");
  check("Y on Flame did not touch the pearl", !u.W.pearlOpened);
}

section("U12 friendly terminal (protocol OFF): the typed-word sniffer");
{
  const t = fresh(); t.W.advance(10);
  t.W.type("chess", 30);
  check("typing chess opens the board", t.chess.isOpen());
  check("the C was swallowed (no CANCEL), H/E/S passed harmlessly", t.W.termSaw.length === 0, JSON.stringify(t.W.termSaw));
  const u = fresh(); u.W.type("ches", 30); u.W.advance(2500); u.W.type("s", 30);
  check("a >2 s pause resets the word", !u.chess.isOpen());
  const v = fresh({ station: "swan" }); v.W.type("chess", 30);
  check("off-Flame: no board, and the 'c' DOES reach CANCEL", !v.chess.isOpen() && v.W.termSaw.indexOf("CANCEL") === 0, JSON.stringify(v.W.termSaw));
  const w = fresh(); const ev = w.W.key("c");
  check("known trade-off: a lone 'c' on the Flame is swallowed, so keyboard CANCEL is dead there", ev._sip === true && w.W.termSaw.length === 0);
}

section("U13 leaving mid-game / re-entry / leaks");
{
  const t = fresh({ protocol: false }); t.chess.open();
  clickSq(t, 52); clickSq(t, 36); t.W.advance(100);
  t.W.key("Escape");
  const timersAfterClose = t.W.timerCount();
  t.chess.open();
  check("re-open starts a FRESH game (the position is not kept)", t.eng.game.st.turn === "w" && t.eng.game.log.length === 0 && sq(t, 36).textContent === "");
  t.W.advance(2000);
  check("the cancelled engine reply never lands on the new game", t.eng.game.st.turn === "w" && t.eng.game.log.length === 0);
  check("no timers pending after close (idle)", timersAfterClose === 0, "pending=" + timersAfterClose);
}
{
  const t = fresh(); t.chess.open(); clickSq(t, 52); clickSq(t, 36); t.W.advance(500);
  t.W.term.setStation("swan");
  check("switching station mid-game closes the board", !t.chess.isOpen());
}

section("U14 prefers-reduced-motion");
{
  const t = fresh({ reducedMotion: true }); t.chess.open();
  setPos(t, "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1"); clickSq(t, 56); clickSq(t, 0); t.W.advance(5);
  check("menu opens immediately", t.eng.menuOn);
  t.eng.runCode("77"); t.W.advance(10);
  check("code 77 prints all lines at once and hands off immediately", t.W.boot.length === 1 && !t.chess.isOpen() && /PURGING SYSTEM/.test(t.eng.el.out.textContent), "boot=" + t.W.boot.length + " out=" + JSON.stringify(t.eng.el.out.textContent));
}

section("U15 game-over handling");
{
  const t = fresh(); t.chess.open();
  setPos(t, "r5k1/8/8/8/8/8/5PPP/6K1 w - - 0 1");
  clickSq(t, 62); clickSq(t, 63); t.W.advance(300);
  check("engine mates the player: CHECKMATE - YOU LOSE, no menu", /YOU LOSE/.test(t.eng.el.status.textContent) && !t.eng.menuOn, t.eng.el.status.textContent);
  clickSq(t, 55);
  check("board is frozen after game over", t.eng.game.over && t.eng.game.sel === -1);
  t.W.click(btn(t, "TAKE BACK"));
  check("TAKE BACK un-ends the game", !t.eng.game.over && t.eng.game.st.turn === "w");
  const s = fresh(); s.chess.open();
  setPos(s, "7k/5Q2/8/8/8/8/8/K7 w - - 0 1");
  clickSq(s, 13); clickSq(s, 22); s.W.advance(10);   // Qf7-g6 -> stalemate? (h8 king: g8,g7,h7 covered)
  info("stalemate attempt status: " + s.eng.el.status.textContent);
  const m = fresh(); m.chess.open();
  setPos(m, "4k3/8/8/8/8/8/8/R3K3 w - - 99 80");
  clickSq(m, 56); clickSq(m, 48); m.W.advance(10);
  check("50-move rule ends the game in the UI", /FIFTY/.test(m.eng.el.status.textContent), m.eng.el.status.textContent);
}

section("U16 nothing in chess touches the flaps");
{
  const t = fresh(); t.chess.open();
  clickSq(t, 52); clickSq(t, 36); t.W.advance(500);
  setPos(t, "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1"); clickSq(t, 56); clickSq(t, 0); t.W.advance(1500);
  t.eng.runCode("77"); t.W.advance(5000);
  check("zero dispatcher commands sent through SwanTerm.send", t.W.sent.length === 0);
  check("no exception anywhere in the run (a missing fetch/XHR global would show here)", t.W.errors.length === 0, t.W.errors[0]);
}

console.log("\ntest_chess_ui: " + nOk + " ok, " + nFail + " FAIL");
if (nFail) console.log(nFail + " failure(s)");
else console.log("all checks passed");
process.exitCode = nFail ? 1 : 0;
